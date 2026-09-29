// Thermal Emergency Manager -- example: a complete synthetic thermal excursion.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// The facility model below is SYNTHETIC. The process, the durable store, the
// filesystem behaviour, and the library calls are REAL.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "tem/clock.hpp"
#include "tem/report.hpp"
#include "tem/runtime.hpp"
#include "tem/store.hpp"

using namespace summon::tem;

namespace {

class Workspace {
 public:
  Workspace() {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path(ec) /
            ("tem-example-" + std::to_string(static_cast<long long>(stamp)));
    std::filesystem::create_directories(root_, ec);
  }
  ~Workspace() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

void note(const char* stage, EmergencyRuntime& runtime, Timestamp now) {
  const ReadModel model = runtime.Inspect(now);
  std::cout << "  " << stage << ": severity=" << severity_name(model.severity)
            << " lifecycle=" << lifecycle_name(model.lifecycle)
            << " requests=" << model.requests.size()
            << " eligible=" << (model.eligibility.eligible ? "yes" : "no") << "\n";
}

}  // namespace

int main() {
  Workspace workspace;
  std::cout << "thermal emergency example (SYNTHETIC facility model, REAL process)\n";
  std::cout << "store: " << workspace.root().string() << "\n";

  SystemClock clock;
  ScriptedTransport transport;
  RuntimeOptions options;
  options.store_root = workspace.root();
  options.incarnation = ControllerIncarnation::from_value(1);
  options.epoch = ControlEpoch::from_value(1);
  options.durability = DurabilityMode::Durable;
  options.transport = &transport;

  auto opened = EmergencyRuntime::Open(options);
  if (!opened.ok()) {
    std::cerr << "open failed: " << opened.status().to_string() << "\n";
    return 1;
  }
  EmergencyRuntime& runtime = *opened.value();

  const auto zone = ThermalZoneRef::parse("zone-a").value();
  const auto probe_a = ThermalProbeRef::parse("zone-a/probe-1").value();
  const auto probe_b = ThermalProbeRef::parse("zone-a/probe-2").value();
  const auto domain = PowerDomainRef::parse("power/domain-a").value();
  const auto cdu = EquipmentRef::parse("equipment/cdu-a/branch-1").value();
  const auto source = SensorSourceRef::parse("synthetic/example-plant").value();

  if (!runtime.RegisterZone(runtime.Authority(), zone).ok() ||
      !runtime.RegisterProbe(runtime.Authority(), zone, probe_a).ok() ||
      !runtime.RegisterProbe(runtime.Authority(), zone, probe_b).ok()) {
    std::cerr << "facility registration failed\n";
    return 1;
  }
  TargetBinding binding;
  binding.zone = zone;
  binding.power_domain = domain;
  binding.equipment.push_back(cdu);
  if (!runtime.RegisterTargetBinding(runtime.Authority(), binding).ok()) {
    std::cerr << "target binding registration failed\n";
    return 1;
  }
  ProtectedObligation interlock;
  interlock.ref = "obligation/keep-evacuation-path-energized";
  interlock.zone = zone;
  interlock.description = "evacuation path power must remain energized";
  interlock.protection = ProtectionClass::HardSafetyInterlock;
  interlock.forbidden_classes = mitigation_class_bit(MitigationClass::ReducePower);
  interlock.authority = ExternalAuthorityRef{std::string("ops/facility-safety")};
  auto obligation = runtime.RegisterObligation(runtime.Authority(), interlock);
  if (!obligation.ok()) {
    std::cerr << "obligation registration failed\n";
    return 1;
  }
  auto reported = runtime.ReportObligationStatus(
      runtime.Authority(), obligation.value(), ObligationStatus::Satisfied,
      ExternalAuthorityRef{std::string("ops/facility-safety")}, clock.Now());
  if (!reported.ok()) {
    std::cerr << "obligation status report failed: " << reported.status().to_string()
              << std::endl;
    return 1;
  }

  Timestamp now = clock.Now();
  std::uint64_t sequence = 1;
  std::int32_t temperature = 22'000;

  const auto emit = [&](std::int32_t celsius_milli) {
    for (const ThermalProbeRef& probe : {probe_a, probe_b}) {
      ThermalSample sample;
      sample.zone = zone;
      sample.probe = probe;
      sample.source = source;
      sample.observed_at = now;
      sample.received_at = now;
      sample.sequence = ObservationSequence::from_value(sequence++);
      sample.has_temperature = true;
      sample.temperature = MilliCelsius::from_value(celsius_milli);
      sample.has_rate = true;
      sample.rate = MilliCelsiusPerMinute::from_value(1'500);
      auto admitted = runtime.AdmitSample(runtime.Authority(), sample);
      if (!admitted.ok()) {
        std::cerr << "sample rejected: " << admitted.status().to_string() << "\n";
      }
    }
  };

  // The owning authority reaffirms obligation status before it ages out; a
  // status that is no longer current is treated as unknown, which blocks
  // recovery by design.
  Timestamp obligation_reported_at = now;
  const auto refresh_obligation = [&]() {
    if (now.unix_nanos() - obligation_reported_at.unix_nanos() <
        Duration::from_minutes(5).nanos()) {
      return;
    }
    (void)runtime.ReportObligationStatus(runtime.Authority(), obligation.value(),
                                         ObligationStatus::Satisfied,
                                         ExternalAuthorityRef{std::string("ops/facility-safety")},
                                         now);
    obligation_reported_at = now;
  };

  std::cout << "\nescalation\n";
  for (int step = 0; step < 24; ++step) {
    now = now.checked_add(Duration::from_seconds(20)).value_or(now);
    refresh_obligation();
    temperature += step < 12 ? 1'500 : -1'800;
    emit(temperature);
    auto report = runtime.Tick(runtime.Authority(), now);
    if (!report.ok()) {
      std::cerr << "tick failed: " << report.status().to_string() << "\n";
      return 1;
    }
    if (step == 0) {
      note("detection", runtime, now);
    }
  }
  note("after escalation and cooling", runtime, now);

  // The adjacent authority confirms the effect of every acknowledged request,
  // and reaffirms confirmations before they age out: verification evidence is
  // only current for a bounded window, and a stale confirmation stops counting.
  const auto confirm_settled = [&]() {
    const ReadModel model = runtime.Inspect(now);
    for (const MitigationRecord& record : model.requests) {
      bool needed = false;
      if (record.state == MitigationState::Acknowledged ||
          record.state == MitigationState::Observed) {
        needed = true;
      } else if (record.state == MitigationState::Verified && record.has_verified_at) {
        needed = now.unix_nanos() - record.verified_at.unix_nanos() >=
                 Duration::from_minutes(5).nanos();
      }
      if (!needed) {
        continue;
      }
      auto reference = ExternalEvidenceRef::parse("plant/confirm/" +
                                                  std::to_string(record.request.id.value()));
      if (reference.ok()) {
        (void)runtime.VerifyRequest(runtime.Authority(), record.request.id, reference.value(), now);
      }
    }
  };
  confirm_settled();
  note("after verification", runtime, now);

  std::cout << "\nrecovery\n" << report::render_eligibility(runtime.EvaluateRecovery(now, false));

  for (int step = 0; step < 60; ++step) {
    now = now.checked_add(Duration::from_seconds(20)).value_or(now);
    refresh_obligation();
    temperature -= 400;
    emit(temperature);
    auto report = runtime.Tick(runtime.Authority(), now);
    if (!report.ok()) {
      std::cerr << "tick failed: " << report.status().to_string() << "\n";
      return 1;
    }
    confirm_settled();
    const ReadModel model = runtime.Inspect(now);
    if (model.lifecycle == Lifecycle::Stabilizing) {
      auto begun = runtime.BeginRecovery(runtime.Authority(),
                                         OperatorRef{std::string("ops/oncall")}, now);
      if (!begun.ok()) {
        std::cerr << "begin recovery failed: " << begun.status().to_string() << "\n";
        return 1;
      }
    }
    if (model.lifecycle == Lifecycle::Recovered &&
        runtime.EvaluateRecovery(now, true).eligible) {
      // Closure requires the recovered state to have held for the closure
      // dwell, so the loop keeps the facility steady until that gate passes.
      break;
    }
  }
  note("after recovery", runtime, now);

  auto closed = runtime.CloseIncident(runtime.Authority(), OperatorRef{std::string("ops/oncall")},
                                      Disposition::Recovered, now);
  if (!closed.ok()) {
    std::cout << "  closure refused: " << closed.status().to_string() << "\n";
  } else {
    note("after closure", runtime, now);
  }

  std::cout << "\nfinal state\n" << report::render_read_model(runtime.Inspect(now));
  std::cout << "\nstore verification\n"
            << report::render_store_verification(runtime.VerifyStore());
  auto replayed = runtime.VerifyReplay();
  if (replayed.ok()) {
    std::cout << report::render_replay(replayed.value());
  } else {
    std::cout << "replay failed: " << replayed.status().to_string() << "\n";
  }
  std::cout << "\ndispatches issued by the synthetic transport: " << transport.dispatch_count()
            << "\n";

  runtime.Shutdown();
  return 0;
}
