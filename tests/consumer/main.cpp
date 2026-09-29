// Thermal Emergency Manager -- out-of-tree downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// This program is built outside the repository against an installed package.
// It exercises a real library lifecycle: open a durable runtime, register a
// facility, admit thermal evidence, escalate, verify mitigation, and confirm
// the store replays.

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "tem/report.hpp"
#include "tem/runtime.hpp"
#include "tem/store.hpp"
#include "tem/version.hpp"

using namespace summon::tem;

int main() {
  std::cout << "downstream consumer linked against ThermalEmergencyManager "
            << version_string() << "\n";

  std::string root = "tem-consumer-store";
  ScriptedTransport transport;
  RuntimeOptions options;
  options.store_root = root;
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
  const auto zone = ThermalZoneRef::parse("zone-consumer").value();
  const auto probe = ThermalProbeRef::parse("zone-consumer/probe-1").value();
  if (!runtime.RegisterZone(runtime.Authority(), zone).ok() ||
      !runtime.RegisterProbe(runtime.Authority(), zone, probe).ok()) {
    std::cerr << "registration failed\n";
    return 1;
  }

  const Timestamp now = Timestamp::from_unix_nanos(1'800'000'000'000'000'000ll);
  ThermalSample sample;
  sample.zone = zone;
  sample.probe = probe;
  sample.source = SensorSourceRef::parse("consumer/source").value();
  sample.observed_at = now;
  sample.received_at = now;
  sample.sequence = ObservationSequence::from_value(1);
  sample.has_temperature = true;
  sample.temperature = MilliCelsius::from_value(40'000);
  sample.has_rate = true;
  sample.rate = MilliCelsiusPerMinute::from_value(1'000);
  auto admitted = runtime.AdmitSample(runtime.Authority(), sample);
  if (!admitted.ok()) {
    std::cerr << "admission failed: " << admitted.status().to_string() << "\n";
    return 1;
  }
  auto ticked = runtime.Tick(runtime.Authority(), now);
  if (!ticked.ok()) {
    std::cerr << "tick failed: " << ticked.status().to_string() << "\n";
    return 1;
  }
  const ReadModel model = runtime.Inspect(now);
  std::cout << "severity after escalation: " << severity_name(model.severity) << "\n";
  if (model.severity != Severity::Critical) {
    std::cerr << "expected critical severity from a 40.000 C reading\n";
    return 1;
  }
  if (model.requests.empty()) {
    std::cerr << "expected a bounded mitigation request\n";
    return 1;
  }
  auto replayed = runtime.VerifyReplay();
  if (!replayed.ok() || !replayed.value().verified) {
    std::cerr << "replay verification failed\n";
    return 1;
  }
  runtime.Shutdown();
  std::filesystem::remove_all(root);
  std::cout << "downstream consumer lifecycle completed successfully\n";
  return 0;
}
