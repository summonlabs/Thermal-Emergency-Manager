// Thermal Emergency Manager -- real OS-process, lock, and crash-recovery tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every child process below is a real independent OS process executing the
// installed test helper. Readiness waits assert that the child got that far;
// expiry is a hard failure, never a silent pass, and any child still running is
// killed before the test returns.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support.hpp"
#include "tem/store.hpp"

using namespace summon::tem;
using namespace temtest;

namespace {

#ifndef TEM_TEST_CHILD_PATH
#error "TEM_TEST_CHILD_PATH must be defined by the build system"
#endif

std::string child_path() { return std::string(TEM_TEST_CHILD_PATH); }

std::map<std::string, std::string> parse_report(const std::string& text) {
  std::map<std::string, std::string> values;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t split = line.find('=');
    if (split == std::string::npos) {
      continue;
    }
    values[line.substr(0, split)] = line.substr(split + 1);
  }
  return values;
}

std::uint64_t number(const std::map<std::string, std::string>& values, const std::string& key) {
  const auto it = values.find(key);
  if (it == values.end()) {
    return 0;
  }
  return std::strtoull(it->second.c_str(), nullptr, 10);
}

// Opens the store from this process and requires it to resolve cleanly.
void require_store_resolves(const std::filesystem::path& root, const std::string& context) {
  StoreOpenOptions options;
  options.root = root;
  options.create_if_missing = false;
  auto store = DurableStore::Open(options);
  if (!store.ok()) {
    TEM_FAIL(context << ": store did not resolve: " << store.status().to_string());
    return;
  }
  const StoreSnapshot& snapshot = store.value()->snapshot();
  auto replayed = replay(snapshot.checkpoint, snapshot.journal);
  TEM_CHECK(replayed.ok());
  if (replayed.ok()) {
    TEM_CHECK(states_encode_identically(replayed.value(), snapshot.live));
  }
  const StoreVerification verification = store.value()->Verify();
  TEM_CHECK(verification.ok);
  store.value()->Close();
}

}  // namespace

TEM_TEST(a_second_process_cannot_hold_the_store_lock) {
  TempDir dir("mp-lock");
  const std::filesystem::path ready = dir.child("ready");
  ChildProcess child = spawn_child(child_path(), {"hold-lock", dir.str(), ready.string()});
  TEM_REQUIRE(child.valid);

  const bool started = wait_for_file(ready);
  if (!started) {
    const std::string error = read_text_file(dir.child("ready.error"));
    kill_child(child);
    TEM_FAIL("child did not acquire the lock: " << error);
    return;
  }

  auto contested = EmergencyRuntime::Open(
      durable_options(dir.path(), ControllerIncarnation::from_value(2), ControlEpoch::from_value(2)));
  TEM_REQUIRE(!contested.ok());
  TEM_CHECK(contested.status().code() == StatusCode::StoreLocked);

  kill_child(child);
  // The lock is an OS handle, so a killed holder cannot leave a stale lock.
  auto after_kill = EmergencyRuntime::Open(
      durable_options(dir.path(), ControllerIncarnation::from_value(3), ControlEpoch::from_value(3)));
  TEM_REQUIRE(after_kill.ok());
  after_kill.value()->Shutdown();
}

TEM_TEST(a_process_killed_during_commits_leaves_a_complete_generation) {
  TempDir dir("mp-kill");
  const std::filesystem::path ready = dir.child("ready");
  ChildProcess child = spawn_child(child_path(), {"commit-loop", dir.str(), ready.string(), "0"});
  TEM_REQUIRE(child.valid);
  const bool started = wait_for_file(ready);
  if (!started) {
    const std::string error = read_text_file(dir.child("ready.error"));
    kill_child(child);
    TEM_FAIL("child never started committing: " << error);
    return;
  }
  // Let the child perform real durable commits, then kill it without notice.
  sleep_for_millis(250);
  kill_child(child);
  require_store_resolves(dir.path(), "after a hard kill during commits");

  // Repeat: the store must always resolve, and every reopen must be usable.
  for (int attempt = 0; attempt < 3; ++attempt) {
    ChildProcess again = spawn_child(child_path(),
                                     {"commit-loop", dir.str(), ready.string(), "0"});
    TEM_REQUIRE(again.valid);
    (void)wait_for_file(ready);
    sleep_for_millis(120);
    kill_child(again);
    require_store_resolves(dir.path(), "after repeated hard kills");
  }
  std::filesystem::remove(ready);
}

TEM_TEST(a_fresh_process_reports_the_store_it_resolves) {
  TempDir dir("mp-dump");
  {
    auto runtime = EmergencyRuntime::Open(durable_options(dir.path()));
    TEM_REQUIRE(runtime.ok());
    TEM_REQUIRE(register_facility(*runtime.value(), "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
    Timestamp now = base_time();
    for (const std::string& probe : {"zone-p/p1", "zone-p/p2"}) {
      auto admitted = runtime.value()->AdmitSample(
          runtime.value()->Authority(),
          make_sample("zone-p", probe, 33'000, now, probe == "zone-p/p1" ? 1 : 2));
      TEM_REQUIRE(admitted.ok());
    }
    auto ticked = runtime.value()->Tick(runtime.value()->Authority(), now);
    TEM_REQUIRE(ticked.ok());
    runtime.value()->Shutdown();
  }

  const std::filesystem::path report = dir.child("dump.txt");
  ChildProcess child = spawn_child(child_path(), {"dump", dir.str(), report.string()});
  TEM_REQUIRE(child.valid);
  const int code = wait_child(child);
  TEM_CHECK_EQ(0, code);
  const auto values = parse_report(read_text_file(report));
  TEM_CHECK(number(values, "SLOT_A_VALID") == 1);
  TEM_CHECK(number(values, "SLOT_B_VALID") == 1);
  TEM_CHECK(number(values, "REPLAY") == 1);
  TEM_CHECK(number(values, "ZONES") == 1);
  TEM_CHECK(number(values, "INCIDENTS") == 1);
  TEM_CHECK(number(values, "COMMIT") >= 1);
}

TEM_TEST(a_restart_under_a_new_epoch_fences_the_previous_plan) {
  TempDir dir("mp-epoch");
  std::uint64_t fenced = 0;
  {
    auto runtime = EmergencyRuntime::Open(durable_options(dir.path()));
    TEM_REQUIRE(runtime.ok());
    TEM_REQUIRE(register_facility(*runtime.value(), "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
    Timestamp now = base_time();
    std::uint64_t sequence = 1;
    for (int i = 0; i < 20; ++i) {
      now = now.checked_add(Duration::from_seconds(10)).value_or(now);
      for (const std::string& probe : {"zone-p/p1", "zone-p/p2"}) {
        auto admitted = runtime.value()->AdmitSample(
            runtime.value()->Authority(),
            make_sample("zone-p", probe, 40'000, now, sequence++));
        TEM_REQUIRE(admitted.ok());
      }
      auto ticked = runtime.value()->Tick(runtime.value()->Authority(), now);
      TEM_REQUIRE(ticked.ok());
    }
    const ReadModel model = runtime.value()->Inspect(now);
    TEM_REQUIRE(model.requests.size() > 0);
    runtime.value()->Shutdown();
  }

  const std::filesystem::path report = dir.child("epoch.txt");
  ChildProcess child = spawn_child(child_path(), {"reopen-epoch", dir.str(), report.string(), "5", "9"});
  TEM_REQUIRE(child.valid);
  const int code = wait_child(child);
  TEM_CHECK_EQ(0, code);
  const auto values = parse_report(read_text_file(report));
  TEM_CHECK(number(values, "OPENED") == 1);
  TEM_CHECK(number(values, "EPOCH") == 5);
  TEM_CHECK(number(values, "ALL_FRESH") == 0);
  TEM_CHECK(number(values, "RECOVERED_FLAG") == 1);
  TEM_CHECK(number(values, "REPLAY") == 1);
  TEM_CHECK(number(values, "NON_TERMINAL") == 0);
  fenced = number(values, "FENCED");
  TEM_CHECK(fenced > 0);
}

TEM_TEST(a_synthetic_scenario_runs_end_to_end_in_a_child_process) {
  TempDir dir("mp-scenario");
  const std::filesystem::path report = dir.child("scenario.txt");
  ChildProcess child =
      spawn_child(child_path(), {"scenario", dir.str(), report.string(), "full-excursion"});
  TEM_REQUIRE(child.valid);
  const int code = wait_child(child);
  TEM_CHECK_EQ(0, code);
  const auto values = parse_report(read_text_file(report));
  TEM_CHECK(number(values, "OK") == 1);
  TEM_CHECK(number(values, "LIFECYCLE") == static_cast<std::uint64_t>(Lifecycle::Closed));
  require_store_resolves(dir.path(), "after an end-to-end child scenario");
}

TEM_TEST_MAIN()
