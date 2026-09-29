// Thermal Emergency Manager -- helper process for multiprocess and crash tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every mode writes a small report file and, where the parent needs to
// synchronise, creates a ready marker. The parent never depends on this
// process exiting: some modes are meant to be killed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "tem/report.hpp"
#include "tem/runtime.hpp"
#include "tem/scenario.hpp"
#include "tem/store.hpp"

using namespace summon::tem;

namespace {

void write_report(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  stream.flush();
}

void touch(const std::string& path) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << "ready\n";
  stream.flush();
}

RuntimeOptions durable_options(const std::string& root, std::uint64_t epoch, std::uint64_t incarnation) {
  RuntimeOptions options;
  options.store_root = root;
  options.epoch = ControlEpoch::from_value(epoch);
  options.incarnation = ControllerIncarnation::from_value(incarnation);
  options.durability = DurabilityMode::Durable;
  options.create_if_missing = true;
  options.adopt_policy_changes = true;
  return options;
}

int hold_lock(const std::string& root, const std::string& ready) {
  auto runtime = EmergencyRuntime::Open(durable_options(root, 1, 1));
  if (!runtime.ok()) {
    write_report(ready + ".error", runtime.status().to_string());
    return 1;
  }
  touch(ready);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

int commit_loop(const std::string& root, const std::string& ready, std::uint64_t limit) {
  auto runtime = EmergencyRuntime::Open(durable_options(root, 1, 1));
  if (!runtime.ok()) {
    write_report(ready + ".error", runtime.status().to_string());
    return 1;
  }
  auto zone = ThermalZoneRef::parse("zone-child");
  auto probe = ThermalProbeRef::parse("zone-child/probe-1");
  auto zone_status = runtime.value()->RegisterZone(runtime.value()->Authority(), zone.value());
  if (!zone_status.ok()) {
    write_report(ready + ".error", zone_status.status().to_string());
    return 1;
  }
  auto probe_status =
      runtime.value()->RegisterProbe(runtime.value()->Authority(), zone.value(), probe.value());
  if (!probe_status.ok()) {
    write_report(ready + ".error", probe_status.status().to_string());
    return 1;
  }
  const Timestamp start = Timestamp::from_unix_nanos(1'800'000'000'000'000'000ll);
  std::uint64_t sequence = 1;
  Timestamp now = start;
  touch(ready);
  for (std::uint64_t i = 0; limit == 0 || i < limit; ++i) {
    now = now.checked_add(Duration::from_seconds(5)).value_or(now);
    ThermalSample sample;
    sample.zone = zone.value();
    sample.probe = probe.value();
    auto source = SensorSourceRef::parse("child/source");
    sample.source = source.value();
    sample.observed_at = now;
    sample.received_at = now;
    sample.sequence = ObservationSequence::from_value(sequence++);
    sample.has_temperature = true;
    sample.temperature = MilliCelsius::from_value(30'000);
    sample.has_rate = true;
    sample.rate = MilliCelsiusPerMinute::from_value(500);
    auto admitted = runtime.value()->AdmitSample(runtime.value()->Authority(), sample);
    if (!admitted.ok()) {
      write_report(ready + ".error", admitted.status().to_string());
      return 1;
    }
    auto ticked = runtime.value()->Tick(runtime.value()->Authority(), now);
    if (!ticked.ok()) {
      write_report(ready + ".error", ticked.status().to_string());
      return 1;
    }
  }
  std::_Exit(0);
}

int dump(const std::string& root, const std::string& report) {
  StoreOpenOptions options;
  options.root = root;
  options.create_if_missing = false;
  auto store = DurableStore::Open(options);
  if (!store.ok()) {
    write_report(report, "OPEN_FAILED " + store.status().to_string());
    return 1;
  }
  const StoreSnapshot& snapshot = store.value()->snapshot();
  const StoreVerification verification = store.value()->Verify();
  std::ostringstream out;
  out << "COMMIT=" << snapshot.commit_sequence.value() << "\n";
  out << "GENERATION=" << snapshot.store_generation.value() << "\n";
  out << "JOURNAL=" << snapshot.journal.size() << "\n";
  out << "DROPPED=" << snapshot.journal_dropped << "\n";
  out << "SLOT_A_VALID=" << (verification.slot_a_valid ? 1 : 0) << "\n";
  out << "SLOT_B_VALID=" << (verification.slot_b_valid ? 1 : 0) << "\n";
  out << "AUTHORITATIVE=" << verification.authoritative_slot << "\n";
  out << "ZONES=" << snapshot.live.zones.size() << "\n";
  out << "INCIDENTS=" << snapshot.live.incidents.size() << "\n";
  out << "REVISION=" << snapshot.live.revision.value() << "\n";
  const IncidentProjection* incident = find_incident(snapshot.live, snapshot.live.current_incident);
  if (incident != nullptr) {
    out << "SEVERITY=" << static_cast<int>(severity_ordinal(incident->severity)) << "\n";
    out << "LIFECYCLE=" << static_cast<int>(lifecycle_ordinal(incident->lifecycle)) << "\n";
    out << "REQUESTS=" << incident->requests.size() << "\n";
  }
  auto replayed = replay(snapshot.checkpoint, snapshot.journal);
  out << "REPLAY=" << (replayed.ok() && states_encode_identically(replayed.value(), snapshot.live) ? 1 : 0)
      << "\n";
  write_report(report, out.str());
  store.value()->Close();
  return 0;
}

int reopen_epoch(const std::string& root, const std::string& report, std::uint64_t epoch,
                 std::uint64_t incarnation) {
  auto runtime = EmergencyRuntime::Open(durable_options(root, epoch, incarnation));
  if (!runtime.ok()) {
    write_report(report, "OPEN_FAILED " + runtime.status().to_string());
    return 1;
  }
  const ReadModel model = runtime.value()->Inspect(Timestamp::from_unix_nanos(1'800'000'001'000'000'000ll));
  std::ostringstream out;
  out << "OPENED=1\n";
  out << "EPOCH=" << model.epoch.value() << "\n";
  out << "SEVERITY=" << static_cast<int>(severity_ordinal(model.severity)) << "\n";
  out << "FENCED=" << model.fenced_requests << "\n";
  out << "ALL_FRESH=" << (model.evidence.all_fresh ? 1 : 0) << "\n";
  out << "RECOVERED_FLAG=" << (model.recovered_from_store ? 1 : 0) << "\n";
  out << "OPEN_REQUESTS=" << model.requests.size() << "\n";
  std::uint32_t open_non_terminal = 0;
  for (const MitigationRecord& record : model.requests) {
    if (!record.is_terminal()) {
      ++open_non_terminal;
    }
  }
  out << "NON_TERMINAL=" << open_non_terminal << "\n";
  auto replayed = runtime.value()->VerifyReplay();
  out << "REPLAY=" << (replayed.ok() ? 1 : 0) << "\n";
  write_report(report, out.str());
  runtime.value()->Shutdown();
  return 0;
}

int run_scenario(const std::string& root, const std::string& report, const std::string& name) {
  auto parsed = scenario_from_name(name);
  if (!parsed.ok()) {
    write_report(report, "BAD_NAME");
    return 2;
  }
  ScenarioConfig config;
  config.start = Timestamp::from_unix_nanos(1'800'000'000'000'000'000ll);
  config.source = SensorSourceRef::parse("synthetic/child").value();
  config.sample_interval = Duration::from_seconds(10);
  config.tick_interval = Duration::from_seconds(10);
  config.verification_delay = Duration::from_seconds(30);
  ZoneSpec zone;
  zone.zone = ThermalZoneRef::parse("zone-child").value();
  zone.probes.push_back(ThermalProbeRef::parse("zone-child/probe-1").value());
  zone.probes.push_back(ThermalProbeRef::parse("zone-child/probe-2").value());
  config.zones.push_back(zone);

  const std::string store_root = root;
  ScenarioRunner runner(
      [store_root](ControllerIncarnation incarnation, ControlEpoch epoch,
                   IMitigationTransport& transport) {
        RuntimeOptions options =
            durable_options(store_root, epoch.value(), incarnation.value());
        options.transport = &transport;
        return EmergencyRuntime::Open(options);
      },
      config);
  auto outcome = runner.Run(parsed.value());
  if (!outcome.ok()) {
    write_report(report, "RUN_FAILED " + outcome.status().to_string());
    return 1;
  }
  std::ostringstream out;
  out << "OK=" << (outcome.value().ok ? 1 : 0) << "\n";
  out << "PEAK=" << static_cast<int>(severity_ordinal(outcome.value().peak_severity)) << "\n";
  out << "FINAL=" << static_cast<int>(severity_ordinal(outcome.value().final_severity)) << "\n";
  out << "LIFECYCLE=" << static_cast<int>(lifecycle_ordinal(outcome.value().final_lifecycle)) << "\n";
  write_report(report, out.str());
  return outcome.value().ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: tem_test_child <mode> <args...>\n";
    return 2;
  }
  const std::string mode = argv[1];
  if (mode == "hold-lock") {
    return hold_lock(argv[2], argc > 3 ? argv[3] : std::string("ready"));
  }
  if (mode == "commit-loop") {
    const std::uint64_t limit = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0;
    return commit_loop(argv[2], argc > 3 ? argv[3] : std::string("ready"), limit);
  }
  if (mode == "dump") {
    return dump(argv[2], argv[3]);
  }
  if (mode == "reopen-epoch") {
    return reopen_epoch(argv[2], argv[3], std::strtoull(argv[4], nullptr, 10),
                        std::strtoull(argv[5], nullptr, 10));
  }
  if (mode == "scenario") {
    return run_scenario(argv[2], argv[3], argv[4]);
  }
  std::cerr << "unknown mode: " << mode << "\n";
  return 2;
}
