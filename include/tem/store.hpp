// Thermal Emergency Manager -- durable, integrity-checked, dual-slot store.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "tem/ids.hpp"
#include "tem/journal.hpp"
#include "tem/state.hpp"
#include "tem/status.hpp"

namespace summon::tem {

// Durable commits are the default. MemoryOnly exists so that deterministic
// semantic tests and completed-operation benchmarks can run without touching
// the filesystem; it is never a production durability mode.
enum class DurabilityMode : std::uint8_t {
  Durable = 1,
  MemoryOnly = 2,
};

// The complete persisted unit.
//
//   checkpoint : the domain state as of the last journal entry that was
//                retired from the retention window
//   journal    : retained authoritative inputs, contiguous and ordered, all
//                with a sequence greater than checkpoint.journal_sequence
//   live       : the domain state after applying the journal to the checkpoint
//
// Invariant verified on load and after every commit:
//   replay(checkpoint, journal) == live, byte for byte.
struct StoreSnapshot {
  StoreGeneration store_generation{};
  CommitSequence commit_sequence{};
  Timestamp committed_at{};
  DomainState checkpoint{};
  DomainState live{};
  std::vector<JournalEntry> journal{};
  // Number of entries retired from the retention window. Store metadata, not
  // domain state: it is deliberately outside the replay equality check.
  std::uint64_t journal_dropped{0};
};

struct StoreOpenOptions {
  std::filesystem::path root{};
  DurabilityMode durability{DurabilityMode::Durable};
  bool create_if_missing{true};
  std::uint64_t max_snapshot_bytes{HardLimits::kMaxSnapshotBytes};
};

struct StoreOpenReport {
  bool created{false};
  bool loaded{false};
  bool previous_generation_recovered{false};
  bool other_slot_unusable{false};
  StoreGeneration generation{};
  CommitSequence commit{};
  std::uint32_t journal_entries{0};
  std::uint32_t incidents{0};
  std::uint32_t zones{0};
  std::string detail{};
};

// Result of an explicit integrity verification of both slots.
struct StoreVerification {
  bool ok{false};
  bool slot_a_present{false};
  bool slot_b_present{false};
  bool slot_a_valid{false};
  bool slot_b_valid{false};
  CommitSequence slot_a_commit{};
  CommitSequence slot_b_commit{};
  StoreGeneration slot_a_generation{};
  StoreGeneration slot_b_generation{};
  std::uint64_t slot_a_bytes{0};
  std::uint64_t slot_b_bytes{0};
  std::uint32_t slot_a_payload_crc{0};
  std::uint32_t slot_b_payload_crc{0};
  std::uint32_t authoritative_slot{0};
  bool authoritative_is_older{false};
  std::string detail{};
};

// One slot file header, decoded without touching the payload.
struct SlotInfo {
  bool present{false};
  bool valid{false};
  std::uint64_t bytes{0};
  std::uint16_t format_version{0};
  StoreGeneration generation{};
  CommitSequence commit{};
  Timestamp committed_at{};
  std::uint64_t payload_length{0};
  std::uint32_t payload_crc{0};
  Status status{};
};

// Exclusive OS-level lock for single-writer authority. The lock is taken on a
// canonicalised lock file inside the store directory and is held for the
// lifetime of the store. Two processes can never hold it at once, and a process
// that dies while holding it cannot leave a stale lock behind, because the
// exclusion is an OS handle rather than a marker file.
class StoreLock {
 public:
  [[nodiscard]] static Result<std::unique_ptr<StoreLock>> Acquire(
      const std::filesystem::path& canonical_root);
  ~StoreLock();
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;

  void Release() noexcept;
  [[nodiscard]] bool held() const noexcept;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  StoreLock() = default;
  std::filesystem::path path_{};
  void* handle_{nullptr};
  bool held_{false};
};

// Canonicalises and validates a store root. Refuses paths that are empty,
// contain NUL, exceed the platform bound, name a non-directory, or are a
// reparse point (symlink or junction) at the root itself.
[[nodiscard]] Result<std::filesystem::path> canonicalize_store_root(
    const std::filesystem::path& path, bool create_if_missing);

// Slot file names inside the store root.
[[nodiscard]] std::filesystem::path slot_path(const std::filesystem::path& root, std::uint32_t slot);
[[nodiscard]] std::filesystem::path stage_path(const std::filesystem::path& root, std::uint32_t slot);
inline constexpr std::uint32_t kSlotCount = 2;

// Reads and validates one slot file without decoding the domain state.
[[nodiscard]] SlotInfo inspect_slot(const std::filesystem::path& path, std::uint64_t max_bytes);

class DurableStore {
 public:
  [[nodiscard]] static Result<std::unique_ptr<DurableStore>> Open(const StoreOpenOptions& options);
  ~DurableStore();
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  // The in-memory authoritative snapshot. Mutated only by the runtime, under
  // the runtime's state lock, and published by Commit().
  [[nodiscard]] StoreSnapshot& snapshot() noexcept { return snapshot_; }
  [[nodiscard]] const StoreSnapshot& snapshot() const noexcept { return snapshot_; }

  // Publishes the snapshot. Sequence: encode, bound-check, stage to a
  // temporary file in the same directory, write, flush to the device, read
  // back and compare, then atomically replace the inactive slot. The commit
  // point is the successful atomic replacement: before it, the previous slot
  // remains authoritative.
  [[nodiscard]] Status Commit(Timestamp committed_at);

  [[nodiscard]] StoreVerification Verify() const;
  [[nodiscard]] const StoreOpenReport& open_report() const noexcept { return report_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] DurabilityMode durability() const noexcept { return options_.durability; }
  [[nodiscard]] std::uint64_t commit_count() const noexcept { return commit_count_; }
  [[nodiscard]] std::uint32_t authoritative_slot() const noexcept { return authoritative_slot_; }
  void Close() noexcept;

 private:
  DurableStore() = default;

  StoreOpenOptions options_{};
  std::filesystem::path root_{};
  std::unique_ptr<StoreLock> lock_{};
  StoreSnapshot snapshot_{};
  StoreOpenReport report_{};
  std::uint32_t authoritative_slot_{0};
  std::uint64_t commit_count_{0};
  bool closed_{false};
};

// Encodes a snapshot into the canonical on-disk representation.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_snapshot(const StoreSnapshot& snapshot,
                                                                std::uint64_t max_bytes);

// Decodes and fully validates a snapshot. Requires exact trailing-byte
// agreement and exact payload length agreement.
[[nodiscard]] Result<StoreSnapshot> decode_snapshot(std::span<const std::uint8_t> bytes,
                                                    std::uint64_t max_bytes);

// On-disk header size and trailer size, exposed for tests and documentation.
inline constexpr std::size_t kSnapshotHeaderBytes = 48;
inline constexpr std::size_t kSnapshotTrailerBytes = 16;

}  // namespace summon::tem
