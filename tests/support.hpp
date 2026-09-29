// Thermal Emergency Manager -- shared test support.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tem/clock.hpp"
#include "tem/report.hpp"
#include "tem/runtime.hpp"
#include "tem/scenario.hpp"
#include "tem/state.hpp"
#include "tem/store.hpp"
#include "tem/transport.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace temtest {

using namespace summon::tem;

// A temporary directory that removes itself. Every test artifact lives here so
// that a test run leaves nothing behind.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    path_ = base / ("tem-test-" + tag + "-" + std::to_string(stamp) + "-" +
                    std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path child(const std::string& name) const {
    return path_ / name;
  }
  [[nodiscard]] std::string str() const { return path_.string(); }

 private:
  std::filesystem::path path_;
};

inline Timestamp base_time() { return Timestamp::from_unix_nanos(1'800'000'000'000'000'000ll); }

inline RuntimeOptions memory_options(ControllerIncarnation incarnation = ControllerIncarnation::from_value(1),
                                     ControlEpoch epoch = ControlEpoch::from_value(1)) {
  RuntimeOptions options;
  options.incarnation = incarnation;
  options.epoch = epoch;
  options.durability = DurabilityMode::MemoryOnly;
  options.create_if_missing = true;
  return options;
}

inline RuntimeOptions durable_options(const std::filesystem::path& root,
                                      ControllerIncarnation incarnation = ControllerIncarnation::from_value(1),
                                      ControlEpoch epoch = ControlEpoch::from_value(1)) {
  RuntimeOptions options;
  options.store_root = root;
  options.incarnation = incarnation;
  options.epoch = epoch;
  options.durability = DurabilityMode::Durable;
  options.create_if_missing = true;
  options.adopt_policy_changes = true;
  return options;
}

inline ThermalSample make_sample(const std::string& zone, const std::string& probe, std::int32_t milli_celsius,
                                 Timestamp at, std::uint64_t sequence, SampleQuality quality = SampleQuality::Good,
                                 MilliCelsiusPerMinute rate = MilliCelsiusPerMinute::from_value(0)) {
  ThermalSample sample;
  auto zone_ref = ThermalZoneRef::parse(zone);
  auto probe_ref = ThermalProbeRef::parse(probe);
  auto source_ref = SensorSourceRef::parse("test/source");
  sample.zone = zone_ref.value();
  sample.probe = probe_ref.value();
  sample.source = source_ref.value();
  sample.quality = quality;
  sample.origin = SampleOrigin::Live;
  sample.observed_at = at;
  sample.received_at = at;
  sample.sequence = ObservationSequence::from_value(sequence);
  if (quality == SampleQuality::Good || quality == SampleQuality::Suspect) {
    sample.has_temperature = true;
    sample.temperature = MilliCelsius::from_value(milli_celsius);
    sample.has_rate = true;
    sample.rate = rate;
  } else {
    sample.has_temperature = false;
    sample.has_rate = false;
  }
  return sample;
}

// Registers one zone with two probes and returns the runtime-ready facility.
inline VoidResult register_facility(EmergencyRuntime& runtime, const std::string& zone,
                                    const std::vector<std::string>& probes) {
  auto zone_ref = ThermalZoneRef::parse(zone);
  if (!zone_ref.ok()) {
    return zone_ref.status();
  }
  auto status = runtime.RegisterZone(runtime.Authority(), zone_ref.value());
  if (!status.ok()) {
    return status;
  }
  for (const std::string& probe : probes) {
    auto probe_ref = ThermalProbeRef::parse(probe);
    if (!probe_ref.ok()) {
      return probe_ref.status();
    }
    status = runtime.RegisterProbe(runtime.Authority(), zone_ref.value(), probe_ref.value());
    if (!status.ok()) {
      return status;
    }
  }
  return VoidResult{};
}

// Independent reference model for the severity ladder. Written from the
// documented rules rather than derived from the implementation, so a
// disagreement between the two is a real signal.
inline Severity reference_severity(std::int32_t max_milli_celsius, const ThermalPolicy& policy) {
  if (max_milli_celsius >= policy.catastrophic_enter.value()) return Severity::Catastrophic;
  if (max_milli_celsius >= policy.emergency_enter.value()) return Severity::Emergency;
  if (max_milli_celsius >= policy.critical_enter.value()) return Severity::Critical;
  if (max_milli_celsius >= policy.warning_enter.value()) return Severity::Warning;
  if (max_milli_celsius >= policy.advisory_enter.value()) return Severity::Advisory;
  return Severity::Nominal;
}

// Deterministic pseudo-random generator with explicit reproduction data.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}
  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }
  std::uint32_t below(std::uint32_t bound) { return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound); }
  std::int32_t range(std::int32_t low, std::int32_t high) {
    return low + static_cast<std::int32_t>(below(static_cast<std::uint32_t>(high - low + 1)));
  }
  [[nodiscard]] std::uint64_t state() const { return state_; }

 private:
  std::uint64_t state_;
};

// A deterministic driving harness: the caller owns the clock, the temperature
// model, and the sample cadence, so every scenario is exactly reproducible.
class Harness {
 public:
  static Result<std::unique_ptr<Harness>> open(DurabilityMode mode,
                                               const std::filesystem::path& root = {}) {
    auto harness = std::unique_ptr<Harness>(new Harness());
    RuntimeOptions options;
    options.incarnation = ControllerIncarnation::from_value(1);
    options.epoch = ControlEpoch::from_value(1);
    options.durability = mode;
    options.create_if_missing = true;
    options.adopt_policy_changes = true;
    options.store_root = root;
    options.transport = &harness->transport;
    auto runtime = EmergencyRuntime::Open(options);
    if (!runtime.ok()) {
      return runtime.status();
    }
    harness->runtime = std::move(runtime).value();
    harness->now = base_time();
    auto started = register_facility(*harness->runtime, harness->zone, harness->probes);
    if (!started.ok()) {
      return started.status();
    }
    return harness;
  }

  Status feed() {
    for (const std::string& probe : probes) {
      ThermalSample sample = make_sample(zone, probe, temperature, now, sequence++,
                                         SampleQuality::Good,
                                         MilliCelsiusPerMinute::from_value(rate));
      auto admitted = runtime->AdmitSample(runtime->Authority(), sample);
      if (!admitted.ok()) {
        return admitted.status();
      }
    }
    return Status::success();
  }

  Result<TickReport> tick() { return runtime->Tick(runtime->Authority(), now); }

  Status step(Duration delta) {
    now = now.checked_add(delta).value_or(now);
    Status fed = feed();
    if (!fed.ok()) {
      return fed;
    }
    auto report = tick();
    if (!report.ok()) {
      return report.status();
    }
    return verify_settled();
  }

  Status run(Duration span, const std::function<void(Harness&)>& mutate = nullptr) {
    const Timestamp end = now.checked_add(span).value_or(now);
    while (now < end) {
      if (mutate) {
        mutate(*this);
      }
      Status stepped = step(Duration::from_seconds(10));
      if (!stepped.ok()) {
        return stepped;
      }
    }
    return Status::success();
  }

  // Confirms acknowledged mitigation after a delay and refreshes verification
  // evidence before it ages out, standing in for the adjacent authority that
  // owns the effect. Verification currency is a designed gate: an old
  // confirmation stops counting.
  Status verify_settled() {
    const ReadModel model = runtime->Inspect(now);
    for (const MitigationRecord& record : model.requests) {
      bool needs_confirmation = false;
      if (record.state == MitigationState::Acknowledged ||
          record.state == MitigationState::Observed) {
        needs_confirmation =
            now.unix_nanos() - record.state_at.unix_nanos() >= Duration::from_seconds(30).nanos();
      } else if (record.state == MitigationState::Verified && record.has_verified_at) {
        needs_confirmation =
            now.unix_nanos() - record.verified_at.unix_nanos() >= Duration::from_minutes(5).nanos();
      }
      if (!needs_confirmation) {
        continue;
      }
      auto reference =
          ExternalEvidenceRef::parse("test/verify/" + std::to_string(record.request.id.value()));
      if (!reference.ok()) {
        return reference.status();
      }
      auto verified =
          runtime->VerifyRequest(runtime->Authority(), record.request.id, reference.value(), now);
      if (!verified.ok()) {
        return verified.status();
      }
    }
    return Status::success();
  }

  [[nodiscard]] ReadModel inspect() const { return runtime->Inspect(now); }

  [[nodiscard]] std::uint64_t count(MitigationState state) const {
    std::uint64_t total = 0;
    for (const MitigationRecord& record : inspect().requests) {
      if (record.state == state) {
        ++total;
      }
    }
    return total;
  }

  std::unique_ptr<EmergencyRuntime> runtime;
  ScriptedTransport transport;
  Timestamp now;
  std::string zone{"zone-a"};
  std::vector<std::string> probes{"zone-a/p1", "zone-a/p2"};
  std::int32_t temperature{22'000};
  std::int32_t rate{0};
  std::uint64_t sequence{1};
};

// ---------------------------------------------------------------------------
// Real OS-process helpers. Used by the multiprocess and crash tests.
// ---------------------------------------------------------------------------

struct ChildProcess {
  void* handle{nullptr};
  std::uint64_t pid{0};
  bool valid{false};
};

inline std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (const char c : argument) {
    if (c == '"') {
      out += "\\\"";
    } else {
      out += c;
    }
  }
  out += "\"";
  return out;
}

inline ChildProcess spawn_child(const std::string& executable, const std::vector<std::string>& arguments) {
  ChildProcess child;
#ifdef _WIN32
  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command += " ";
    command += quote_argument(argument);
  }
  std::vector<wchar_t> wide(command.size() + 1);
  std::size_t converted = 0;
  const std::string narrow_command = command;
  mbstowcs_s(&converted, wide.data(), wide.size(), narrow_command.c_str(), narrow_command.size());
  std::vector<wchar_t> wide_executable(executable.size() + 1);
  mbstowcs_s(&converted, wide_executable.data(), wide_executable.size(), executable.c_str(),
             executable.size());

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  const BOOL created = CreateProcessW(wide_executable.data(), wide.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
  if (created == 0) {
    return child;
  }
  CloseHandle(info.hThread);
  child.handle = info.hProcess;
  child.pid = static_cast<std::uint64_t>(info.dwProcessId);
  child.valid = true;
#else
  const pid_t pid = fork();
  if (pid == 0) {
    std::vector<char*> argv;
    std::string exe = executable;
    argv.push_back(exe.data());
    std::vector<std::string> storage = arguments;
    for (std::string& value : storage) {
      argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    execv(executable.c_str(), argv.data());
    _exit(127);
  }
  if (pid < 0) {
    return child;
  }
  child.pid = static_cast<std::uint64_t>(pid);
  child.valid = true;
#endif
  return child;
}

inline int wait_child(const ChildProcess& child) {
  if (!child.valid) {
    return -1;
  }
#ifdef _WIN32
  WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code);
  CloseHandle(static_cast<HANDLE>(child.handle));
  return static_cast<int>(code);
#else
  int status = 0;
  waitpid(static_cast<pid_t>(child.pid), &status, 0);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

inline void kill_child(const ChildProcess& child) {
  if (!child.valid) {
    return;
  }
#ifdef _WIN32
  TerminateProcess(static_cast<HANDLE>(child.handle), 3);
  WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  CloseHandle(static_cast<HANDLE>(child.handle));
#else
  kill(static_cast<pid_t>(child.pid), SIGKILL);
  int status = 0;
  waitpid(static_cast<pid_t>(child.pid), &status, 0);
#endif
}

inline void release_child(const ChildProcess& child) {
  if (!child.valid) {
    return;
  }
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(child.handle));
#endif
}

inline std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

inline void write_text_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
}

// Waits for a child to publish its readiness file. Expiry is a hard failure of
// the calling test, never a silent pass, and the caller always kills the child
// afterwards so no process is left behind.
inline bool wait_for_file(const std::filesystem::path& path, int attempts = 3000) {
  for (int i = 0; i < attempts; ++i) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

inline void sleep_for_millis(int millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

}  // namespace temtest
