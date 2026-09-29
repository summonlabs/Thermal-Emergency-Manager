// Thermal Emergency Manager -- persistence, corruption, and recovery tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support.hpp"
#include "tem/codec.hpp"
#include "tem/store.hpp"

using namespace summon::tem;
using namespace temtest;

namespace {

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

// Drives a durable store through a number of committed operations.
Status run_durable_workload(EmergencyRuntime& runtime, int ticks) {
  Timestamp now = base_time();
  std::uint64_t sequence = 1;
  for (int i = 0; i < ticks; ++i) {
    now = now.checked_add(Duration::from_seconds(10)).value_or(now);
    for (const std::string& probe : {"zone-p/p1", "zone-p/p2"}) {
      auto admitted = runtime.AdmitSample(
          runtime.Authority(),
          make_sample("zone-p", probe, 22'000 + (i % 5) * 1'000, now, sequence++));
      if (!admitted.ok()) {
        return admitted.status();
      }
    }
    auto report = runtime.Tick(runtime.Authority(), now);
    if (!report.ok()) {
      return report.status();
    }
  }
  return Status::success();
}

Result<std::unique_ptr<EmergencyRuntime>> open_durable(const std::filesystem::path& root,
                                                       std::uint64_t epoch = 1,
                                                       std::uint64_t incarnation = 1) {
  return EmergencyRuntime::Open(
      durable_options(root, ControllerIncarnation::from_value(incarnation),
                      ControlEpoch::from_value(epoch)));
}

}  // namespace

TEM_TEST(snapshot_round_trip_is_exact) {
  TempDir dir("roundtrip");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 12).ok());
  runtime->Shutdown();
  runtime.reset();

  StoreOpenOptions options;
  options.root = dir.path();
  options.create_if_missing = false;
  auto store = DurableStore::Open(options);
  TEM_REQUIRE(store.ok());

  auto encoded = encode_snapshot(store.value()->snapshot(), HardLimits::kMaxSnapshotBytes);
  TEM_REQUIRE(encoded.ok());
  auto decoded = decode_snapshot(encoded.value(), HardLimits::kMaxSnapshotBytes);
  TEM_REQUIRE(decoded.ok());
  auto reencoded = encode_snapshot(decoded.value(), HardLimits::kMaxSnapshotBytes);
  TEM_REQUIRE(reencoded.ok());
  TEM_CHECK(encoded.value() == reencoded.value());
  TEM_CHECK(states_encode_identically(decoded.value().live, store.value()->snapshot().live));

  const StoreVerification verification = store.value()->Verify();
  TEM_CHECK(verification.ok);
  TEM_CHECK(verification.slot_a_valid && verification.slot_b_valid);
  store.value()->Close();
}

TEM_TEST(single_byte_corruption_is_detected) {
  TempDir dir("corrupt");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 6).ok());
  runtime->Shutdown();
  runtime.reset();

  const std::filesystem::path slot = slot_path(dir.path(), 0);
  const std::vector<std::uint8_t> original = read_bytes(slot);
  TEM_REQUIRE(original.size() > 128);

  // Payload corruption is caught by the payload checksum.
  std::vector<std::uint8_t> corrupted = original;
  corrupted[original.size() / 2] ^= 0x40;
  write_bytes(slot, corrupted);
  auto info = inspect_slot(slot, HardLimits::kMaxSnapshotBytes);
  TEM_CHECK(!info.valid);
  TEM_CHECK(info.status.code() == StatusCode::StoreIntegrityFailure);

  // Header corruption is caught by the header checksum.
  corrupted = original;
  corrupted[20] ^= 0x01;
  write_bytes(slot, corrupted);
  info = inspect_slot(slot, HardLimits::kMaxSnapshotBytes);
  TEM_CHECK(!info.valid);
  TEM_CHECK(info.status.code() == StatusCode::StoreIntegrityFailure);
}

TEM_TEST(structural_corruption_is_rejected_with_specific_codes) {
  TempDir dir("structural");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 4).ok());
  runtime->Shutdown();
  runtime.reset();

  const std::filesystem::path slot = slot_path(dir.path(), 0);
  const std::vector<std::uint8_t> original = read_bytes(slot);

  {
    std::vector<std::uint8_t> bad = original;
    bad[0] = 'X';
    auto decoded = decode_snapshot(bad, HardLimits::kMaxSnapshotBytes);
    TEM_REQUIRE(!decoded.ok());
    TEM_CHECK(decoded.status().code() == StatusCode::StoreCorrupt);
  }
  {
    std::vector<std::uint8_t> bad = original;
    bad[8] = 9;
    bad[9] = 0;
    auto decoded = decode_snapshot(bad, HardLimits::kMaxSnapshotBytes);
    TEM_REQUIRE(!decoded.ok());
    TEM_CHECK(decoded.status().code() == StatusCode::StoreVersionUnsupported);
  }
  {
    std::vector<std::uint8_t> bad = original;
    // Recompute the header checksum so that the reserved field is what fails.
    bad[10] = 1;
    const std::uint32_t crc = codec::crc32c(std::span<const std::uint8_t>(bad.data(), 44));
    for (int i = 0; i < 4; ++i) {
      bad[44 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu);
    }
    auto decoded = decode_snapshot(bad, HardLimits::kMaxSnapshotBytes);
    TEM_REQUIRE(!decoded.ok());
    TEM_CHECK(decoded.status().code() == StatusCode::ReservedFieldNotZero);
  }
  {
    const std::vector<std::uint8_t> truncated(original.begin(), original.end() - 8);
    auto decoded = decode_snapshot(truncated, HardLimits::kMaxSnapshotBytes);
    TEM_REQUIRE(!decoded.ok());
    TEM_CHECK(decoded.status().code() == StatusCode::StoreTruncated ||
               decoded.status().code() == StatusCode::StoreIntegrityFailure);
  }
  {
    std::vector<std::uint8_t> extended = original;
    extended.push_back(0);
    auto decoded = decode_snapshot(extended, HardLimits::kMaxSnapshotBytes);
    TEM_REQUIRE(!decoded.ok());
    TEM_CHECK(decoded.status().code() == StatusCode::StoreTrailingBytes);
  }
  {
    // A declared payload length beyond the configured bound is refused before
    // any allocation happens.
    auto decoded = decode_snapshot(original, 1024);
    TEM_REQUIRE(!decoded.ok());
    TEM_CHECK(decoded.status().code() == StatusCode::BoundsExceeded);
  }
}

TEM_TEST(a_corrupt_authoritative_slot_falls_back_to_the_previous_generation) {
  TempDir dir("fallback");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 5).ok());
  const CommitSequence before = runtime->Inspect(base_time()).commit_sequence;
  runtime->Shutdown();
  runtime.reset();

  // Corrupt the newest slot file; the store must resolve to the previous
  // complete generation rather than a hybrid.
  const std::filesystem::path newest = slot_path(dir.path(), 1);
  const std::filesystem::path previous = slot_path(dir.path(), 0);
  TEM_REQUIRE(std::filesystem::exists(newest));
  TEM_REQUIRE(std::filesystem::exists(previous));
  const SlotInfo previous_before = inspect_slot(previous, HardLimits::kMaxSnapshotBytes);
  TEM_REQUIRE(previous_before.valid);
  std::vector<std::uint8_t> bytes = read_bytes(newest);
  bytes[bytes.size() / 2] ^= 0xFF;
  write_bytes(newest, bytes);

  // The store itself must resolve to the previous complete generation and say
  // so explicitly, before any runtime writes anything.
  {
    StoreOpenOptions options;
    options.root = dir.path();
    options.create_if_missing = false;
    auto store = DurableStore::Open(options);
    TEM_REQUIRE_OK(store);
    TEM_CHECK(store.value()->open_report().previous_generation_recovered);
    TEM_CHECK(store.value()->open_report().other_slot_unusable);
    TEM_CHECK_EQ(previous_before.commit.value(),
                 store.value()->snapshot().commit_sequence.value());
    store.value()->Close();
  }

  auto reopened = open_durable(dir.path(), 2, 2);
  TEM_REQUIRE_OK(reopened);
  const ReadModel model = reopened.value()->Inspect(base_time());
  // The reopened runtime has itself committed the recovery entries, so the
  // commit counter has advanced from the recovered generation.
  TEM_CHECK(previous_before.commit.value() < model.commit_sequence.value());
  TEM_CHECK(before.value() < model.commit_sequence.value());
  TEM_CHECK(model.commit_sequence.value() >= 1);
  const StoreVerification verification = reopened.value()->VerifyStore();
  TEM_CHECK(verification.ok);
  auto replayed = reopened.value()->VerifyReplay();
  TEM_REQUIRE(replayed.ok());
  TEM_CHECK(replayed.value().verified);
  reopened.value()->Shutdown();
}

TEM_TEST(no_slot_with_a_usable_generation_is_reported) {
  TempDir dir("noGeneration");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 3).ok());
  runtime->Shutdown();
  runtime.reset();

  for (std::uint32_t slot = 0; slot < kSlotCount; ++slot) {
    std::vector<std::uint8_t> bytes = read_bytes(slot_path(dir.path(), slot));
    bytes[64] ^= 0xAA;
    write_bytes(slot_path(dir.path(), slot), bytes);
  }
  auto reopened = open_durable(dir.path(), 2, 2);
  TEM_REQUIRE(!reopened.ok());
  TEM_CHECK(reopened.status().code() == StatusCode::StoreCorrupt);
}

TEM_TEST(a_staged_file_never_becomes_authoritative) {
  TempDir dir("staged");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 4).ok());
  runtime->Shutdown();
  runtime.reset();

  // A leftover staging file with garbage must be ignored entirely.
  write_bytes(stage_path(dir.path(), 0), std::vector<std::uint8_t>(256, 0xAB));
  write_bytes(stage_path(dir.path(), 1), std::vector<std::uint8_t>(64, 0xCD));

  auto reopened = open_durable(dir.path(), 2, 2);
  TEM_REQUIRE(reopened.ok());
  auto replayed = reopened.value()->VerifyReplay();
  TEM_REQUIRE(replayed.ok());
  TEM_CHECK(replayed.value().verified);
  reopened.value()->Shutdown();
}

TEM_TEST(single_writer_lock_is_exclusive_and_canonical) {
  TempDir dir("lock");
  auto first = open_durable(dir.path());
  TEM_REQUIRE(first.ok());

  auto second = open_durable(dir.path(), 2, 2);
  TEM_REQUIRE(!second.ok());
  TEM_CHECK(second.status().code() == StatusCode::StoreLocked);

  // A different spelling of the same directory resolves to the same lock.
  const std::filesystem::path aliased = dir.path() / "sub" / ".." / ".";
  auto third = open_durable(aliased, 3, 3);
  TEM_REQUIRE(!third.ok());
  TEM_CHECK(third.status().code() == StatusCode::StoreLocked);

  first.value()->Shutdown();
  {
    std::unique_ptr<EmergencyRuntime> released = std::move(first).value();
    released.reset();
  }
  auto fourth = open_durable(dir.path(), 4, 4);
  TEM_REQUIRE(fourth.ok());
  fourth.value()->Shutdown();
}

TEM_TEST(invalid_store_paths_are_refused) {
  auto empty = canonicalize_store_root(std::filesystem::path(), true);
  TEM_REQUIRE(!empty.ok());
  TEM_CHECK(empty.status().code() == StatusCode::StorePathInvalid);

  TempDir dir("paths");
  const std::filesystem::path file_path = dir.child("not-a-directory.txt");
  write_text_file(file_path, "content");
  auto not_dir = canonicalize_store_root(file_path, true);
  TEM_REQUIRE(!not_dir.ok());
  TEM_CHECK(not_dir.status().code() == StatusCode::StorePathInvalid);

  const std::filesystem::path missing = dir.child("missing").string() + std::string(1, '\0') + "x";
  auto nul_path = canonicalize_store_root(missing, true);
  TEM_REQUIRE(!nul_path.ok());
  TEM_CHECK(nul_path.status().code() == StatusCode::StorePathInvalid);

  auto absent = canonicalize_store_root(dir.child("absent"), false);
  TEM_REQUIRE(!absent.ok());
  TEM_CHECK(absent.status().code() == StatusCode::StoreNotFound);
}

TEM_TEST(journal_retention_advances_the_checkpoint_and_keeps_replay_exact) {
  TempDir dir("retention");
  RuntimeOptions options = durable_options(dir.path());
  options.policy.bounds.max_journal_entries = 8;
  options.policy.bounds.max_transitions_per_incident = 64;
  auto opened_runtime = EmergencyRuntime::Open(options);
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 20).ok());

  const RuntimeStats stats = runtime->Stats();
  TEM_CHECK(stats.journal_dropped > 0);
  auto replayed = runtime->VerifyReplay();
  TEM_REQUIRE(replayed.ok());
  TEM_CHECK(replayed.value().verified);
  TEM_CHECK(replayed.value().checkpoint_revision.value() > 0);
  runtime->Shutdown();
  runtime.reset();

  auto reopened = open_durable(dir.path(), 2, 2);
  TEM_REQUIRE(reopened.ok());
  auto replayed_again = reopened.value()->VerifyReplay();
  TEM_REQUIRE(replayed_again.ok());
  TEM_CHECK(replayed_again.value().verified);
  reopened.value()->Shutdown();
}

TEM_TEST(restored_observations_are_not_current_evidence_until_revalidated) {
  TempDir dir("recovered");
  auto opened_runtime = open_durable(dir.path());
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  Timestamp now = base_time();
  for (const std::string& probe : {"zone-p/p1", "zone-p/p2"}) {
    auto admitted = runtime->AdmitSample(
        runtime->Authority(),
        make_sample("zone-p", probe, 41'000, now, probe == "zone-p/p1" ? 1 : 2));
    TEM_REQUIRE(admitted.ok());
  }
  auto ticked = runtime->Tick(runtime->Authority(), now);
  TEM_REQUIRE(ticked.ok());
  const ReadModel before = runtime->Inspect(now);
  TEM_REQUIRE(severity_ordinal(before.severity) >= severity_ordinal(Severity::Critical));
  runtime->Shutdown();
  runtime.reset();

  // A new incarnation in a new epoch reopens the store.
  auto reopened = open_durable(dir.path(), 2, 2);
  TEM_REQUIRE_OK(reopened);
  const ReadModel restored = reopened.value()->Inspect(now);
  TEM_CHECK(restored.recovered_from_store);
  TEM_CHECK(!restored.evidence.all_fresh);
  for (const ZoneAssessment& zone : restored.evidence.zones) {
    TEM_CHECK(zone.cls != FreshnessClass::Fresh);
  }
  TEM_CHECK(!restored.eligibility.eligible);
  TEM_CHECK(restored.severity == before.severity);

  // A live sample from each probe restores currency.
  Timestamp later = now.checked_add(Duration::from_seconds(10)).value_or(now);
  for (const std::string& probe : {"zone-p/p1", "zone-p/p2"}) {
    auto admitted = reopened.value()->AdmitSample(
        reopened.value()->Authority(),
        make_sample("zone-p", probe, 41'000, later, probe == "zone-p/p1" ? 3 : 4));
    TEM_REQUIRE(admitted.ok());
  }
  auto retick = reopened.value()->Tick(reopened.value()->Authority(), later);
  TEM_REQUIRE(retick.ok());
  const ReadModel refreshed = reopened.value()->Inspect(later);
  TEM_CHECK(refreshed.evidence.all_fresh);
  TEM_CHECK(refreshed.severity == before.severity);
  reopened.value()->Shutdown();
}

TEM_TEST(volatile_store_writes_nothing_to_disk) {
  TempDir dir("volatile");
  RuntimeOptions options = memory_options();
  options.store_root = dir.path();
  auto opened_runtime = EmergencyRuntime::Open(options);
  TEM_REQUIRE(opened_runtime.ok());
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened_runtime).value();
  TEM_REQUIRE(register_facility(*runtime, "zone-p", {"zone-p/p1", "zone-p/p2"}).ok());
  TEM_REQUIRE(run_durable_workload(*runtime, 3).ok());
  TEM_CHECK(runtime->Stats().commits > 0);
  TEM_CHECK(!std::filesystem::exists(slot_path(dir.path(), 0)));
  TEM_CHECK(!std::filesystem::exists(slot_path(dir.path(), 1)));
  const StoreVerification verification = runtime->VerifyStore();
  TEM_CHECK(verification.ok);
  runtime->Shutdown();
}

TEM_TEST_MAIN()
