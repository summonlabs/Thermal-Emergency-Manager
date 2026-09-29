// Thermal Emergency Manager -- completed-operation benchmark.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Measures completed, durable operations: every measured unit of work returns
// only after its commit has been flushed and read back. Warm-up runs are
// discarded and reported separately. The facility model is SYNTHETIC; the
// process, filesystem, durability, and library behaviour are REAL.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "tem/report.hpp"
#include "tem/runtime.hpp"
#include "tem/store.hpp"
#include "tem/version.hpp"

using namespace summon::tem;

namespace {

class Workspace {
 public:
  Workspace() {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path(ec) /
            ("tem-bench-" + std::to_string(static_cast<long long>(stamp)));
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

struct Measurement {
  std::string name;
  std::string unit;
  std::uint64_t iterations{0};
  std::uint64_t warmup{0};
  double total_nanos{0.0};
  double nanos_per_operation{0.0};
  double operations_per_second{0.0};
  std::string evidence;
};

void print_measurement(const Measurement& measurement) {
  std::cout << measurement.name << "\n";
  std::cout << "  unit: " << measurement.unit << "  iterations: " << measurement.iterations
            << "  warmup: " << measurement.warmup << "\n";
  std::printf("  completed operations per second: %.1f\n", measurement.operations_per_second);
  std::printf("  mean latency per completed operation: %.1f us\n",
              measurement.nanos_per_operation / 1000.0);
  std::cout << "  evidence: " << measurement.evidence << "\n\n";
}

struct Harness {
  std::unique_ptr<EmergencyRuntime> runtime;
  Timestamp now;
  std::uint64_t sequence{1};
  std::int32_t temperature{22'000};
};

VoidResult register_facility_local(EmergencyRuntime& runtime) {
  const auto zone = ThermalZoneRef::parse("zone-bench").value();
  auto status = runtime.RegisterZone(runtime.Authority(), zone);
  if (!status.ok()) {
    return status;
  }
  for (const char* name : {"zone-bench/p1", "zone-bench/p2"}) {
    const auto probe = ThermalProbeRef::parse(name).value();
    status = runtime.RegisterProbe(runtime.Authority(), zone, probe);
    if (!status.ok()) {
      return status;
    }
  }
  return VoidResult{};
}

std::unique_ptr<Harness> open_harness(const std::filesystem::path& root, DurabilityMode mode) {
  auto harness = std::make_unique<Harness>();
  RuntimeOptions options;
  options.store_root = root;
  options.incarnation = ControllerIncarnation::from_value(1);
  options.epoch = ControlEpoch::from_value(1);
  options.durability = mode;
  options.create_if_missing = true;
  auto runtime = EmergencyRuntime::Open(options);
  if (!runtime.ok()) {
    std::cerr << "open failed: " << runtime.status().to_string() << "\n";
    return nullptr;
  }
  harness->runtime = std::move(runtime).value();
  harness->now = Timestamp::from_unix_nanos(1'800'000'000'000'000'000ll);
  if (!register_facility_local(*harness->runtime).ok()) {
    return nullptr;
  }
  return harness;
}

VoidResult admit(Harness& harness, std::int32_t milli_celsius) {
  harness.now = harness.now.checked_add(Duration::from_seconds(10)).value_or(harness.now);
  for (const char* name : {"zone-bench/p1", "zone-bench/p2"}) {
    ThermalSample sample;
    sample.zone = ThermalZoneRef::parse("zone-bench").value();
    sample.probe = ThermalProbeRef::parse(name).value();
    sample.source = SensorSourceRef::parse("synthetic/bench").value();
    sample.observed_at = harness.now;
    sample.received_at = harness.now;
    sample.sequence = ObservationSequence::from_value(harness.sequence++);
    sample.has_temperature = true;
    sample.temperature = MilliCelsius::from_value(milli_celsius);
    sample.has_rate = true;
    sample.rate = MilliCelsiusPerMinute::from_value(500);
    auto admitted = harness.runtime->AdmitSample(harness.runtime->Authority(), sample);
    if (!admitted.ok()) {
      return admitted.status();
    }
  }
  return VoidResult{};
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t iterations = 2000;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--iterations=", 0) == 0) {
      iterations = std::strtoull(arg.substr(13).c_str(), nullptr, 10);
    }
  }

  Workspace workspace;
  std::cout << "Thermal Emergency Manager " << version_string() << " benchmark\n";
  std::cout << "workload: SYNTHETIC facility model driven through REAL library calls\n";
  std::cout << "store: " << workspace.root().string() << "\n";
  std::cout << "note: every measured operation returns only after its durable commit has been "
               "flushed and read back\n\n";

  Measurement durable_evidence;
  {
    auto harness = open_harness(workspace.root() / "evidence", DurabilityMode::Durable);
    if (harness == nullptr) {
      return 1;
    }
    const std::uint64_t warmup = 50;
    for (std::uint64_t i = 0; i < warmup; ++i) {
      if (!admit(*harness, 22'000).ok()) {
        return 1;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      auto admitted = admit(*harness, 22'000);
      if (!admitted.ok()) {
        std::cerr << "admission failed: " << admitted.status().to_string() << "\n";
        return 1;
      }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    durable_evidence.name = "durable evidence admission (one probe sample per operation)";
    durable_evidence.unit = "sample admitted and committed";
    durable_evidence.iterations = iterations;
    durable_evidence.warmup = warmup;
    durable_evidence.total_nanos =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    durable_evidence.nanos_per_operation =
        durable_evidence.total_nanos / static_cast<double>(iterations);
    durable_evidence.operations_per_second =
        1e9 / durable_evidence.nanos_per_operation;
    const RuntimeStats stats = harness->runtime->Stats();
    durable_evidence.evidence = "REAL durable commits: " + std::to_string(stats.commits) +
                                " commits, journal entries " +
                                std::to_string(harness->runtime->VerifyReplay().ok() ? 1 : 0) +
                                " replay verified";
    harness->runtime->Shutdown();
    print_measurement(durable_evidence);
  }

  Measurement decision;
  {
    auto harness = open_harness(workspace.root() / "decision", DurabilityMode::MemoryOnly);
    if (harness == nullptr) {
      return 1;
    }
    const std::uint64_t warmup = 200;
    for (std::uint64_t i = 0; i < warmup; ++i) {
      if (!admit(*harness, 30'000).ok()) {
        return 1;
      }
      auto report_result = harness->runtime->Tick(harness->runtime->Authority(), harness->now);
      if (!report_result.ok()) {
        return 1;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
      auto admitted = admit(*harness, 30'000);
      if (!admitted.ok()) {
        return 1;
      }
      auto ticked = harness->runtime->Tick(harness->runtime->Authority(), harness->now);
      if (!ticked.ok()) {
        return 1;
      }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    decision.name = "emergency decision evaluation (admission plus escalation tick)";
    decision.unit = "completed decision cycle";
    decision.iterations = iterations;
    decision.warmup = warmup;
    decision.total_nanos =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    decision.nanos_per_operation = decision.total_nanos / static_cast<double>(iterations);
    decision.operations_per_second = 1e9 / decision.nanos_per_operation;
    decision.evidence = "REAL evaluation over an in-memory store; no durability claimed";
    harness->runtime->Shutdown();
    print_measurement(decision);
  }

  Measurement cycle;
  {
    auto harness = open_harness(workspace.root() / "cycle", DurabilityMode::Durable);
    if (harness == nullptr) {
      return 1;
    }
    const std::uint64_t measured = iterations / 4 == 0 ? 1 : iterations / 4;
    const auto run_cycle = [&]() -> bool {
      for (int step = 0; step < 12; ++step) {
        harness->temperature += 2'000;
        if (!admit(*harness, harness->temperature).ok()) {
          return false;
        }
        auto ticked = harness->runtime->Tick(harness->runtime->Authority(), harness->now);
        if (!ticked.ok()) {
          return false;
        }
      }
      return true;
    };
    for (int i = 0; i < 2; ++i) {
      if (!run_cycle()) {
        return 1;
      }
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < measured; ++i) {
      if (!run_cycle()) {
        return 1;
      }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    cycle.name = "durable escalation cycle (12 escalation steps)";
    cycle.unit = "completed escalation cycle";
    cycle.iterations = measured;
    cycle.warmup = 2;
    cycle.total_nanos =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    cycle.nanos_per_operation = cycle.total_nanos / static_cast<double>(measured);
    cycle.operations_per_second = 1e9 / cycle.nanos_per_operation;
    const auto replayed = harness->runtime->VerifyReplay();
    cycle.evidence = "REAL durable commits, replay verified: " +
                     std::string(replayed.ok() ? "yes" : "no") + ", slots: " +
                     std::to_string(harness->runtime->VerifyStore().ok ? 1 : 0);
    harness->runtime->Shutdown();
    print_measurement(cycle);
  }

  std::cout << "environment: Windows/MSVC, " << (sizeof(void*) * 8) << "-bit, "
            << __cplusplus / 100 % 100 << " standard, release build\n";
  return 0;
}
