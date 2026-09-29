// Thermal Emergency Manager -- administration and inspection CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// The CLI only uses the public library API. It is an operator-facing inspection
// surface, not a second implementation of the runtime.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "tem/clock.hpp"
#include "tem/report.hpp"
#include "tem/runtime.hpp"
#include "tem/scenario.hpp"
#include "tem/store.hpp"
#include "tem/version.hpp"

namespace {

using namespace summon::tem;

struct Arguments {
  std::string command;
  std::string store;
  std::string name;
  std::uint64_t epoch{0};
  std::uint64_t incarnation{0};
  bool keep{false};
  bool all{false};
  bool help{false};
};

void print_usage() {
  std::cout
      << "temctl " << version_banner() << "\n"
      << "usage: temctl <command> [options]\n\n"
      << "commands:\n"
      << "  version                     print the build and store format version\n"
      << "  demo [--store DIR] [--keep] run a durable synthetic excursion end to end\n"
      << "  scenario --name NAME        run one named synthetic scenario, or --all\n"
      << "  inspect --store DIR         read a durable store and print its state\n"
      << "  verify --store DIR          verify slot integrity and journal replay\n"
      << "  help                        print this message\n\n"
      << "options:\n"
      << "  --store DIR       store directory (default: a temporary directory)\n"
      << "  --name NAME       scenario name; use --all for every scenario\n"
      << "  --all             run every synthetic scenario\n"
      << "  --keep            keep the temporary store directory after the command\n";
}

Result<Arguments> parse(int argc, char** argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string token = argv[i];
    if (token == "--store") {
      if (i + 1 >= argc) {
        return Status::error(StatusCode::InvalidArgument, "--store requires a directory");
      }
      args.store = argv[++i];
    } else if (token == "--name") {
      if (i + 1 >= argc) {
        return Status::error(StatusCode::InvalidArgument, "--name requires a scenario name");
      }
      args.name = argv[++i];
    } else if (token == "--epoch") {
      if (i + 1 >= argc) {
        return Status::error(StatusCode::InvalidArgument, "--epoch requires a value");
      }
      args.epoch = std::strtoull(argv[++i], nullptr, 10);
    } else if (token == "--incarnation") {
      if (i + 1 >= argc) {
        return Status::error(StatusCode::InvalidArgument, "--incarnation requires a value");
      }
      args.incarnation = std::strtoull(argv[++i], nullptr, 10);
    } else if (token == "--keep") {
      args.keep = true;
    } else if (token == "--all") {
      args.all = true;
    } else if (token == "--help" || token == "-h") {
      args.help = true;
    } else if (!token.empty() && token[0] == '-') {
      return Status::error(StatusCode::InvalidArgument, "unknown option").with_context(token);
    } else if (args.command.empty()) {
      args.command = token;
    } else {
      return Status::error(StatusCode::InvalidArgument, "unexpected argument").with_context(token);
    }
  }
  if (args.command.empty() && !args.help) {
    args.command = "help";
  }
  return args;
}

class TempDirectory {
 public:
  explicit TempDirectory(std::string prefix) : path_(make(prefix)) {}
  ~TempDirectory() {
    if (keep_) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  void keep() { keep_ = true; }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  static std::filesystem::path make(const std::string& prefix) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path candidate =
        base / (prefix + "-" + std::to_string(static_cast<unsigned long long>(now)));
    std::filesystem::create_directories(candidate, ec);
    return candidate;
  }

  std::filesystem::path path_;
  bool keep_{false};
};

ScenarioConfig default_config(Timestamp start, std::string zone_name) {
  ScenarioConfig config;
  config.start = start;
  config.source = SensorSourceRef{std::string("synthetic/plant-a")};
  config.sample_interval = Duration::from_seconds(10);
  config.tick_interval = Duration::from_seconds(10);
  config.verification_delay = Duration::from_seconds(30);

  ZoneSpec zone;
  auto parsed_zone = ThermalZoneRef::parse(zone_name);
  zone.zone = parsed_zone.ok() ? parsed_zone.value() : ThermalZoneRef{std::string("zone-a")};
  auto probe_a = ThermalProbeRef::parse("zone-a/probe-1");
  auto probe_b = ThermalProbeRef::parse("zone-a/probe-2");
  zone.probes.push_back(probe_a.value());
  zone.probes.push_back(probe_b.value());
  auto domain = PowerDomainRef::parse("power/domain-a");
  zone.power_domain = domain.value();
  auto equipment = EquipmentRef::parse("equipment/cdu-a/branch-1");
  zone.equipment.push_back(equipment.value());
  zone.initial = MilliCelsius::from_value(22'000);
  zone.ramp = MilliCelsiusPerMinute::from_value(0);
  config.zones.push_back(zone);

  ObligationSpec interlock;
  interlock.ref = "obligation/keep-evacuation-path-energized";
  interlock.zone = zone.zone;
  interlock.description = "evacuation path lighting and egress power must stay energized";
  interlock.protection = ProtectionClass::HardSafetyInterlock;
  interlock.forbidden_classes = mitigation_class_bit(MitigationClass::ReducePower);
  interlock.authority = ExternalAuthorityRef{std::string("ops/facility-safety")};
  config.obligations.push_back(interlock);

  ObligationSpec advisory;
  advisory.ref = "obligation/advisory-fan-curve";
  advisory.zone = zone.zone;
  advisory.description = "advisory fan-curve optimisation constraint";
  advisory.protection = ProtectionClass::AdvisoryOptimization;
  advisory.forbidden_classes = mitigation_class_bit(MitigationClass::DerateAccelerators);
  config.obligations.push_back(advisory);
  return config;
}

ScenarioRunner::RuntimeFactory durable_factory(const std::filesystem::path& root) {
  return [root](ControllerIncarnation incarnation, ControlEpoch epoch,
                IMitigationTransport& transport) {
    RuntimeOptions options;
    options.store_root = root;
    options.incarnation = incarnation;
    options.epoch = epoch;
    options.durability = DurabilityMode::Durable;
    options.create_if_missing = true;
    options.adopt_policy_changes = true;
    options.transport = &transport;
    // The emergency authority may relax advisory optimisation constraints, and
    // only those; the runtime refuses everything else by construction.
    options.relaxation.relaxable_classes = 0x0F;
    return EmergencyRuntime::Open(options);
  };
}

int run_scenario(const std::string& name, const std::filesystem::path& root, bool all) {
  // A fixed start in the past keeps an inspection under the real clock reading
  // the modelled evidence as stale rather than as a future reading.
  const Timestamp start = Timestamp::from_unix_nanos(1'577'836'800'000'000'000ll);
  std::vector<ScenarioId> ids;
  if (all) {
    ids = all_scenarios();
  } else {
    auto parsed = scenario_from_name(name.empty() ? std::string("full-excursion") : name);
    if (!parsed.ok()) {
      std::cerr << parsed.status().to_string() << "\n";
      return 2;
    }
    ids.push_back(parsed.value());
  }

  int failures = 0;
  for (const ScenarioId id : ids) {
    const std::filesystem::path scenario_root = root / std::string(scenario_name(id));
    ScenarioConfig config = default_config(start, "zone-a");
    ScenarioRunner runner(durable_factory(scenario_root), config);
    auto outcome = runner.Run(id);
    if (!outcome.ok()) {
      std::cerr << "scenario " << scenario_name(id) << " failed: "
                << outcome.status().to_string() << "\n";
      ++failures;
      continue;
    }
    const ScenarioOutcome& result = outcome.value();
    std::cout << "scenario " << result.name << ": " << (result.ok ? "EXPECTATIONS MET" : "FAILED")
              << "\n";
    std::cout << "  peak severity=" << severity_name(result.peak_severity)
              << " final severity=" << severity_name(result.final_severity)
              << " lifecycle=" << lifecycle_name(result.final_lifecycle) << "\n";
    std::cout << "  samples=" << result.samples << " ticks=" << result.ticks
              << " dispatches=" << result.dispatches << " restarts=" << result.restarts << "\n";
    std::cout << "  requests issued=" << result.requests_issued
              << " verified=" << result.requests_verified
              << " failed=" << result.requests_failed
              << " abandoned=" << result.requests_abandoned
              << " superseded=" << result.requests_superseded << "\n";
    if (!result.ok) {
      std::cout << "  failure: " << result.failure_detail << "\n";
      std::cout << "  requests:\n" << report::render_requests(result.final_model.requests);
      std::cout << report::render_eligibility(result.final_model.eligibility);
      ++failures;
    }
    for (const std::string& event : result.timeline) {
      std::cout << "    " << event << "\n";
    }
  }
  return failures == 0 ? 0 : 1;
}

int run_demo(const std::filesystem::path& root) {
  // A fixed start in the past keeps an inspection under the real clock reading
  // the modelled evidence as stale rather than as a future reading.
  const Timestamp start = Timestamp::from_unix_nanos(1'577'836'800'000'000'000ll);
  ScenarioConfig config = default_config(start, "zone-a");
  ScenarioRunner runner(durable_factory(root), config);
  auto outcome = runner.Run(ScenarioId::FullExcursion);
  if (!outcome.ok()) {
    std::cerr << "demo failed: " << outcome.status().to_string() << "\n";
    return 1;
  }
  const ScenarioOutcome& result = outcome.value();
  std::cout << "synthetic thermal excursion (SYNTHETIC facility model; REAL process, filesystem, "
               "and package behaviour)\n";
  std::cout << "store: " << root.string() << "\n\n";
  std::cout << "timeline:\n";
  for (const std::string& event : result.timeline) {
    std::cout << "  " << event << "\n";
  }
  std::cout << "\nfinal inspection:\n" << report::render_read_model(result.final_model);
  std::cout << "\nresult: peak=" << severity_name(result.peak_severity)
            << " lifecycle=" << lifecycle_name(result.final_lifecycle)
            << " dispatches=" << result.dispatches
            << " verified=" << result.requests_verified << "\n";
  return result.ok ? 0 : 1;
}

int run_inspect(const std::filesystem::path& root) {
  StoreOpenOptions options;
  options.root = root;
  options.create_if_missing = false;
  options.durability = DurabilityMode::Durable;
  auto store = DurableStore::Open(options);
  if (!store.ok()) {
    std::cerr << store.status().to_string() << "\n";
    return 2;
  }
  const StoreSnapshot& snapshot = store.value()->snapshot();
  SystemClock clock;
  RuntimeStats stats;
  ReadModel model = build_read_model(snapshot.live, snapshot, clock.Now(), stats);
  model.durability = store.value()->durability();
  const std::u8string root_text = store.value()->root().u8string();
  model.store_root = std::string(reinterpret_cast<const char*>(root_text.data()), root_text.size());
  std::cout << report::render_read_model(model);
  std::cout << "journal entries retained: " << snapshot.journal.size()
            << " (dropped " << snapshot.journal_dropped << ")\n";
  std::cout << "journal tail:\n" << report::render_journal(snapshot.journal, 12);
  store.value()->Close();
  return 0;
}

int run_verify(const std::filesystem::path& root) {
  StoreOpenOptions options;
  options.root = root;
  options.create_if_missing = false;
  auto store = DurableStore::Open(options);
  if (!store.ok()) {
    std::cerr << store.status().to_string() << "\n";
    return 2;
  }
  const StoreVerification verification = store.value()->Verify();
  std::cout << report::render_store_verification(verification);
  const StoreSnapshot& snapshot = store.value()->snapshot();
  auto replayed = replay(snapshot.checkpoint, snapshot.journal);
  if (!replayed.ok()) {
    std::cerr << "replay failed: " << replayed.status().to_string() << "\n";
    store.value()->Close();
    return 1;
  }
  ReplayReport report;
  report.journal_entries = static_cast<std::uint32_t>(snapshot.journal.size());
  report.checkpoint_revision = snapshot.checkpoint.revision;
  report.live_revision = snapshot.live.revision;
  report.reconstructed_revision = replayed.value().revision;
  report.incidents = static_cast<std::uint32_t>(snapshot.live.incidents.size());
  const IncidentProjection* incident =
      find_incident(snapshot.live, snapshot.live.current_incident);
  report.requests = incident != nullptr ? static_cast<std::uint32_t>(incident->requests.size()) : 0;
  report.verified = states_encode_identically(replayed.value(), snapshot.live);
  report.detail = report.verified ? "replay reproduced the live state exactly"
                                  : "replayed state differs from the live state";
  std::cout << report::render_replay(report);
  store.value()->Close();
  return (verification.ok && report.verified) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  auto parsed = parse(argc, argv);
  if (!parsed.ok()) {
    std::cerr << parsed.status().to_string() << "\n";
    print_usage();
    return 2;
  }
  const Arguments& args = parsed.value();
  if (args.help || args.command == "help") {
    print_usage();
    return 0;
  }
  if (args.command == "version") {
    std::cout << version_banner() << "\n";
    return 0;
  }

  const bool needs_store = args.command == "demo" || args.command == "inspect" ||
                           args.command == "verify" || args.command == "scenario";
  TempDirectory temp("temctl");
  std::filesystem::path root;
  bool temporary = false;
  if (!args.store.empty()) {
    root = args.store;
  } else if (needs_store) {
    root = temp.path();
    temporary = true;
  }
  if (args.keep && temporary) {
    temp.keep();
  }

  if (args.command == "demo") {
    return run_demo(root);
  }
  if (args.command == "scenario") {
    return run_scenario(args.name, root, args.all);
  }
  if (args.command == "inspect") {
    if (args.store.empty()) {
      std::cerr << "inspect requires --store\n";
      return 2;
    }
    return run_inspect(root);
  }
  if (args.command == "verify") {
    if (args.store.empty()) {
      std::cerr << "verify requires --store\n";
      return 2;
    }
    return run_verify(root);
  }
  std::cerr << "unknown command: " << args.command << "\n";
  print_usage();
  return 2;
}
