// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/store.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "tem/codec.hpp"
#include "tem/version.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace summon::tem {
namespace {

constexpr char kSnapshotMagic[8] = {'T', 'E', 'M', 'S', 'T', 'R', '0', '1'};
constexpr char kTrailerMagic[8] = {'T', 'E', 'M', 'E', 'N', 'D', '0', '1'};
constexpr const char* kLockFileName = "tem.lock";
constexpr const char* kSlotFileNames[2] = {"tem-slot-0.tem", "tem-slot-1.tem"};
constexpr const char* kStageFileNames[2] = {"tem-slot-0.tem.tmp", "tem-slot-1.tem.tmp"};
constexpr std::size_t kMaxPathLength = 4096;
constexpr std::size_t kWriteChunk = 1u << 20;

std::string narrow(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

Status io_error(const char* what, const std::filesystem::path& path, std::uint32_t code) {
  Status status = Status::error(StatusCode::StoreIoError, what);
  status.with_context(narrow(path) + " (code " + std::to_string(code) + ")");
  return status;
}

std::uint32_t last_error_code() {
#ifdef _WIN32
  return static_cast<std::uint32_t>(::GetLastError());
#else
  return static_cast<std::uint32_t>(errno);
#endif
}

bool path_contains_nul(const std::filesystem::path& path) {
  const auto& native = path.native();
  return std::find(native.begin(), native.end(), typename std::filesystem::path::value_type{}) !=
         native.end();
}

#ifdef _WIN32
bool is_reparse_point(const std::filesystem::path& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
bool is_reparse_point(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec));
}
#endif

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                            std::uint64_t max_bytes) {
#ifdef _WIN32
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("cannot open file for reading", path, last_error_code());
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    const std::uint32_t code = last_error_code();
    ::CloseHandle(handle);
    return io_error("cannot determine file size", path, code);
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    ::CloseHandle(handle);
    return Status::error(StatusCode::BoundsExceeded, "file exceeds the readable bound")
        .with_context(narrow(path));
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const DWORD request = static_cast<DWORD>(
        std::min<std::size_t>(buffer.size() - offset, static_cast<std::size_t>(0x10000000)));
    DWORD read = 0;
    if (::ReadFile(handle, buffer.data() + offset, request, &read, nullptr) == 0) {
      const std::uint32_t code = last_error_code();
      ::CloseHandle(handle);
      return io_error("cannot read file", path, code);
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  ::CloseHandle(handle);
  buffer.resize(offset);
  return buffer;
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return io_error("cannot open file for reading", path, last_error_code());
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const std::uint32_t code = last_error_code();
    ::close(fd);
    return io_error("cannot determine file size", path, code);
  }
  if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    ::close(fd);
    return Status::error(StatusCode::BoundsExceeded, "file exceeds the readable bound")
        .with_context(narrow(path));
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const ssize_t read = ::read(fd, buffer.data() + offset, buffer.size() - offset);
    if (read < 0) {
      const std::uint32_t code = last_error_code();
      ::close(fd);
      return io_error("cannot read file", path, code);
    }
    if (read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(fd);
  buffer.resize(offset);
  return buffer;
#endif
}

// Writes the buffer, flushes it to the device, and closes the handle. A
// successful return means the bytes reached the device, not merely a cache.
Status write_file_durable(const std::filesystem::path& path,
                          const std::vector<std::uint8_t>& bytes) {
#ifdef _WIN32
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("cannot create staging file", path, last_error_code());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD request = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, kWriteChunk));
    DWORD written = 0;
    if (::WriteFile(handle, bytes.data() + offset, request, &written, nullptr) == 0) {
      const std::uint32_t code = last_error_code();
      ::CloseHandle(handle);
      return io_error("cannot write staging file", path, code);
    }
    if (written == 0) {
      ::CloseHandle(handle);
      return io_error("write made no progress", path, 0);
    }
    offset += written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    const std::uint32_t code = last_error_code();
    ::CloseHandle(handle);
    return io_error("cannot flush staging file", path, code);
  }
  ::CloseHandle(handle);
  return Status::success();
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return io_error("cannot create staging file", path, last_error_code());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      const std::uint32_t code = last_error_code();
      ::close(fd);
      return io_error("cannot write staging file", path, code);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    const std::uint32_t code = last_error_code();
    ::close(fd);
    return io_error("cannot flush staging file", path, code);
  }
  ::close(fd);
  return Status::success();
#endif
}

// Atomically replaces target with staged. This is the commit point of the
// store: before it returns, the previous slot file is still authoritative.
Status atomic_replace(const std::filesystem::path& staged,
                      const std::filesystem::path& target) {
#ifdef _WIN32
  if (::MoveFileExW(staged.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return io_error("cannot publish the staged slot", target, last_error_code());
  }
  return Status::success();
#else
  if (::rename(staged.c_str(), target.c_str()) != 0) {
    return io_error("cannot publish the staged slot", target, last_error_code());
  }
  return Status::success();
#endif
}

void remove_file(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void sync_directory(const std::filesystem::path& root) {
#ifndef _WIN32
  const int fd = ::open(root.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#else
  (void)root;
#endif
}

std::uint32_t read_u32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  }
  return value;
}

std::int64_t read_i64(const std::uint8_t* data) {
  const std::uint64_t raw = read_u64(data);
  std::int64_t value = 0;
  std::memcpy(&value, &raw, sizeof(value));
  return value;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void store_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

}  // namespace

std::filesystem::path slot_path(const std::filesystem::path& root, std::uint32_t slot) {
  return root / kSlotFileNames[slot % kSlotCount];
}

std::filesystem::path stage_path(const std::filesystem::path& root, std::uint32_t slot) {
  return root / kStageFileNames[slot % kSlotCount];
}

Result<std::filesystem::path> canonicalize_store_root(const std::filesystem::path& path,
                                                      bool create_if_missing) {
  if (path.empty()) {
    return Status::error(StatusCode::StorePathInvalid, "store root path is empty");
  }
  if (path_contains_nul(path)) {
    return Status::error(StatusCode::StorePathInvalid, "store root path contains a NUL character");
  }
  if (path.native().size() > kMaxPathLength) {
    return Status::error(StatusCode::StorePathInvalid, "store root path is too long")
        .with_context(narrow(path));
  }
  std::error_code ec;
  std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (ec) {
    return Status::error(StatusCode::StorePathInvalid, "store root path cannot be resolved")
        .with_context(ec.message());
  }
  std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
  if (ec) {
    return Status::error(StatusCode::StorePathInvalid, "store root path cannot be canonicalised")
        .with_context(ec.message());
  }

  const bool exists = std::filesystem::exists(canonical, ec);
  if (ec) {
    return Status::error(StatusCode::StorePathInvalid, "store root path cannot be inspected")
        .with_context(ec.message());
  }
  if (exists) {
    if (!std::filesystem::is_directory(canonical, ec) || ec) {
      return Status::error(StatusCode::StorePathInvalid, "store root is not a directory")
          .with_context(narrow(canonical));
    }
    if (is_reparse_point(canonical)) {
      return Status::error(StatusCode::StorePathInvalid,
                           "store root is a symbolic link or reparse point")
          .with_context(narrow(canonical));
    }
    return canonical;
  }
  if (!create_if_missing) {
    return Status::error(StatusCode::StoreNotFound, "store root does not exist")
        .with_context(narrow(canonical));
  }
  if (!std::filesystem::create_directories(canonical, ec) || ec) {
    return Status::error(StatusCode::StoreIoError, "store root cannot be created")
        .with_context(ec ? ec.message() : std::string("unknown error"));
  }
  std::filesystem::path created = std::filesystem::weakly_canonical(canonical, ec);
  if (ec) {
    return Status::error(StatusCode::StorePathInvalid, "created store root cannot be canonicalised")
        .with_context(ec.message());
  }
  return created;
}

Result<std::unique_ptr<StoreLock>> StoreLock::Acquire(const std::filesystem::path& canonical_root) {
  const std::filesystem::path lock_path = canonical_root / kLockFileName;
  auto lock = std::unique_ptr<StoreLock>(new StoreLock());
  lock->path_ = lock_path;

#ifdef _WIN32
  const HANDLE handle =
      ::CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const std::uint32_t code = last_error_code();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return Status::error(StatusCode::StoreLocked,
                           "another runtime already holds the single-writer lock")
          .with_context(narrow(lock_path));
    }
    return io_error("cannot acquire the store lock", lock_path, code);
  }
  lock->handle_ = handle;
#else
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    return io_error("cannot open the store lock", lock_path, last_error_code());
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const std::uint32_t code = last_error_code();
    ::close(fd);
    if (code == EWOULDBLOCK) {
      return Status::error(StatusCode::StoreLocked,
                           "another runtime already holds the single-writer lock")
          .with_context(narrow(lock_path));
    }
    return io_error("cannot acquire the store lock", lock_path, code);
  }
  lock->handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
#endif
  lock->held_ = true;
  return lock;
}

StoreLock::~StoreLock() { Release(); }

void StoreLock::Release() noexcept {
  if (!held_) {
    return;
  }
#ifdef _WIN32
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
  }
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (fd >= 0) {
    ::flock(fd, LOCK_UN);
    ::close(fd);
  }
#endif
  handle_ = nullptr;
  held_ = false;
}

bool StoreLock::held() const noexcept { return held_; }

SlotInfo inspect_slot(const std::filesystem::path& path, std::uint64_t max_bytes) {
  SlotInfo info;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    info.status = Status::error(StatusCode::StoreNotFound, "slot file is absent");
    return info;
  }
  info.present = true;
  auto bytes = read_file(path, max_bytes + kSnapshotHeaderBytes + kSnapshotTrailerBytes);
  if (!bytes.ok()) {
    info.status = bytes.status();
    return info;
  }
  const std::vector<std::uint8_t>& data = bytes.value();
  info.bytes = data.size();
  if (data.size() < kSnapshotHeaderBytes + kSnapshotTrailerBytes) {
    info.status = Status::error(StatusCode::StoreTruncated, "slot file is shorter than a header");
    return info;
  }
  if (std::memcmp(data.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
    info.status = Status::error(StatusCode::StoreCorrupt, "slot magic does not match");
    return info;
  }
  info.format_version = static_cast<std::uint16_t>(data[8] | (data[9] << 8));
  if (info.format_version != kStoreFormatVersion) {
    info.status = Status::error(StatusCode::StoreVersionUnsupported, "slot format version differs");
    return info;
  }
  if (data[10] != 0 || data[11] != 0) {
    info.status = Status::error(StatusCode::ReservedFieldNotZero,
                                "slot header reserved field is not zero");
    return info;
  }
  const std::uint32_t header_crc = read_u32(data.data() + 44);
  const std::uint32_t computed_header_crc =
      codec::crc32c(std::span<const std::uint8_t>(data.data(), 44));
  if (header_crc != computed_header_crc) {
    info.status = Status::error(StatusCode::StoreIntegrityFailure, "slot header checksum differs");
    return info;
  }
  info.generation = StoreGeneration::from_value(read_u64(data.data() + 12));
  info.commit = CommitSequence::from_value(read_u64(data.data() + 20));
  info.committed_at = Timestamp::from_unix_nanos(read_i64(data.data() + 28));
  info.payload_length = read_u32(data.data() + 36);
  info.payload_crc = read_u32(data.data() + 40);
  if (info.payload_length > max_bytes) {
    info.status = Status::error(StatusCode::BoundsExceeded, "slot payload exceeds the bound");
    return info;
  }
  if (data.size() != kSnapshotHeaderBytes + info.payload_length + kSnapshotTrailerBytes) {
    info.status = Status::error(StatusCode::StoreTruncated,
                                "slot file length does not match its declared payload");
    return info;
  }
  const std::uint8_t* trailer = data.data() + kSnapshotHeaderBytes + info.payload_length;
  if (std::memcmp(trailer, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
    info.status = Status::error(StatusCode::StoreCorrupt, "slot trailer magic does not match");
    return info;
  }
  if (read_u64(trailer + 8) != data.size()) {
    info.status = Status::error(StatusCode::StoreTrailingBytes,
                                "slot trailer length does not match the file length");
    return info;
  }
  const std::uint32_t payload_crc = codec::crc32c(
      std::span<const std::uint8_t>(data.data() + kSnapshotHeaderBytes, info.payload_length));
  if (payload_crc != info.payload_crc) {
    info.status = Status::error(StatusCode::StoreIntegrityFailure, "slot payload checksum differs");
    return info;
  }
  info.valid = true;
  info.status = Status::success();
  return info;
}

Result<std::vector<std::uint8_t>> encode_snapshot(const StoreSnapshot& snapshot,
                                                  std::uint64_t max_bytes) {
  codec::Writer payload;
  const std::vector<std::uint8_t> checkpoint = encode_state(snapshot.checkpoint);
  const std::vector<std::uint8_t> live = encode_state(snapshot.live);
  if (checkpoint.size() > HardLimits::kMaxSnapshotBytes ||
      live.size() > HardLimits::kMaxSnapshotBytes) {
    return Status::error(StatusCode::BoundsExceeded, "encoded state exceeds the snapshot bound");
  }
  payload.u32(static_cast<std::uint32_t>(checkpoint.size()));
  payload.raw(checkpoint);
  payload.u32(static_cast<std::uint32_t>(live.size()));
  payload.raw(live);
  payload.u32(static_cast<std::uint32_t>(snapshot.journal.size()));
  payload.u64(snapshot.journal_dropped);
  for (const JournalEntry& entry : snapshot.journal) {
    codec::Writer entry_writer;
    codec::encode(entry_writer, entry);
    if (!entry_writer.ok()) {
      return entry_writer.error();
    }
    payload.u32(static_cast<std::uint32_t>(entry_writer.size()));
    payload.raw(entry_writer.bytes());
  }
  if (!payload.ok()) {
    return payload.error();
  }
  if (payload.size() > max_bytes) {
    return Status::error(StatusCode::BoundsExceeded, "snapshot exceeds the configured bound");
  }

  std::vector<std::uint8_t> out;
  out.reserve(kSnapshotHeaderBytes + payload.size() + kSnapshotTrailerBytes);
  out.insert(out.end(), kSnapshotMagic, kSnapshotMagic + sizeof(kSnapshotMagic));
  out.push_back(static_cast<std::uint8_t>(kStoreFormatVersion & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((kStoreFormatVersion >> 8) & 0xFFu));
  out.push_back(0);
  out.push_back(0);
  append_u64(out, snapshot.store_generation.value());
  append_u64(out, snapshot.commit_sequence.value());
  append_u64(out, static_cast<std::uint64_t>(snapshot.committed_at.unix_nanos()));
  append_u32(out, static_cast<std::uint32_t>(payload.size()));
  append_u32(out, codec::crc32c(payload.bytes()));
  append_u32(out, codec::crc32c(std::span<const std::uint8_t>(out.data(), 44)));
  out.insert(out.end(), payload.bytes().begin(), payload.bytes().end());
  out.insert(out.end(), kTrailerMagic, kTrailerMagic + sizeof(kTrailerMagic));
  append_u64(out, out.size() + 8);
  return out;
}

Result<StoreSnapshot> decode_snapshot(std::span<const std::uint8_t> bytes, std::uint64_t max_bytes) {
  if (bytes.size() < kSnapshotHeaderBytes + kSnapshotTrailerBytes) {
    return Status::error(StatusCode::StoreTruncated, "snapshot is shorter than a header");
  }
  if (std::memcmp(bytes.data(), kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
    return Status::error(StatusCode::StoreCorrupt, "snapshot magic does not match");
  }
  const std::uint16_t version = static_cast<std::uint16_t>(bytes[8] | (bytes[9] << 8));
  if (version != kStoreFormatVersion) {
    return Status::error(StatusCode::StoreVersionUnsupported,
                         "snapshot format version is not supported");
  }
  if (bytes[10] != 0 || bytes[11] != 0) {
    return Status::error(StatusCode::ReservedFieldNotZero,
                         "snapshot header reserved field is not zero");
  }
  if (read_u32(bytes.data() + 44) != codec::crc32c(bytes.subspan(0, 44))) {
    return Status::error(StatusCode::StoreIntegrityFailure, "snapshot header checksum differs");
  }
  const std::uint64_t payload_length = read_u32(bytes.data() + 36);
  const std::uint32_t payload_crc = read_u32(bytes.data() + 40);
  if (payload_length > max_bytes) {
    return Status::error(StatusCode::BoundsExceeded, "snapshot payload exceeds the bound");
  }
  if (bytes.size() < kSnapshotHeaderBytes + payload_length + kSnapshotTrailerBytes) {
    return Status::error(StatusCode::StoreTruncated, "snapshot payload is truncated");
  }
  if (bytes.size() > kSnapshotHeaderBytes + payload_length + kSnapshotTrailerBytes) {
    return Status::error(StatusCode::StoreTrailingBytes, "snapshot has trailing bytes");
  }
  const std::uint8_t* trailer = bytes.data() + kSnapshotHeaderBytes + payload_length;
  if (std::memcmp(trailer, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
    return Status::error(StatusCode::StoreCorrupt, "snapshot trailer magic does not match");
  }
  if (read_u64(trailer + 8) != bytes.size()) {
    return Status::error(StatusCode::StoreCorrupt,
                         "snapshot trailer length does not match the file length");
  }
  const std::span<const std::uint8_t> payload = bytes.subspan(kSnapshotHeaderBytes, payload_length);
  if (codec::crc32c(payload) != payload_crc) {
    return Status::error(StatusCode::StoreIntegrityFailure, "snapshot payload checksum differs");
  }

  StoreSnapshot snapshot;
  snapshot.store_generation = StoreGeneration::from_value(read_u64(bytes.data() + 12));
  snapshot.commit_sequence = CommitSequence::from_value(read_u64(bytes.data() + 20));
  snapshot.committed_at = Timestamp::from_unix_nanos(read_i64(bytes.data() + 28));

  codec::Reader reader(payload);
  std::size_t checkpoint_size = 0;
  {
    const std::uint32_t declared = reader.u32();
    if (!reader.ok()) {
      return reader.error();
    }
    if (declared > HardLimits::kMaxSnapshotBytes) {
      return Status::error(StatusCode::BoundsExceeded, "checkpoint size exceeds the hard limit");
    }
    checkpoint_size = declared;
  }
  const std::span<const std::uint8_t> checkpoint_bytes = reader.raw(checkpoint_size);
  if (!reader.ok()) {
    return reader.error();
  }
  {
    codec::Reader checkpoint_reader(checkpoint_bytes);
    codec::decode(checkpoint_reader, snapshot.checkpoint);
    if (!checkpoint_reader.ok()) {
      return checkpoint_reader.error();
    }
    if (!checkpoint_reader.at_end()) {
      return Status::error(StatusCode::StoreTrailingBytes,
                           "checkpoint encoding has trailing bytes");
    }
  }

  std::uint32_t live_size = 0;
  {
    live_size = reader.u32();
    if (!reader.ok()) {
      return reader.error();
    }
    if (live_size > HardLimits::kMaxSnapshotBytes) {
      return Status::error(StatusCode::BoundsExceeded, "live state size exceeds the hard limit");
    }
  }
  const std::span<const std::uint8_t> live_bytes = reader.raw(live_size);
  if (!reader.ok()) {
    return reader.error();
  }
  {
    codec::Reader live_reader(live_bytes);
    codec::decode(live_reader, snapshot.live);
    if (!live_reader.ok()) {
      return live_reader.error();
    }
    if (!live_reader.at_end()) {
      return Status::error(StatusCode::StoreTrailingBytes, "live encoding has trailing bytes");
    }
  }

  const std::uint32_t journal_count = reader.u32();
  if (!reader.ok()) {
    return reader.error();
  }
  if (journal_count > HardLimits::kMaxJournalEntries) {
    return Status::error(StatusCode::BoundsExceeded, "journal count exceeds the hard limit");
  }
  snapshot.journal_dropped = reader.u64();
  if (!reader.ok()) {
    return reader.error();
  }
  snapshot.journal.reserve(journal_count);
  JournalSequence previous = snapshot.checkpoint.journal_sequence;
  for (std::uint32_t i = 0; i < journal_count; ++i) {
    const std::uint32_t entry_size = reader.u32();
    if (!reader.ok()) {
      return reader.error();
    }
    if (entry_size > HardLimits::kMaxSnapshotBytes) {
      return Status::error(StatusCode::BoundsExceeded, "journal entry exceeds the hard limit");
    }
    const std::span<const std::uint8_t> entry_bytes = reader.raw(entry_size);
    if (!reader.ok()) {
      return reader.error();
    }
    codec::Reader entry_reader(entry_bytes);
    JournalEntry entry;
    codec::decode(entry_reader, entry);
    if (!entry_reader.ok()) {
      return entry_reader.error();
    }
    if (!entry_reader.at_end()) {
      return Status::error(StatusCode::StoreTrailingBytes, "journal entry has trailing bytes");
    }
    if (entry.sequence.value() != previous.value() + 1u) {
      return Status::error(StatusCode::StoreCorrupt,
                           "journal entries are not contiguous with the checkpoint");
    }
    previous = entry.sequence;
    snapshot.journal.push_back(std::move(entry));
  }
  if (!reader.at_end()) {
    return Status::error(StatusCode::StoreTrailingBytes, "snapshot payload has trailing bytes");
  }

  const Status checkpoint_limits = validate_state_limits(snapshot.checkpoint);
  if (!checkpoint_limits.ok()) {
    return checkpoint_limits;
  }
  const Status live_limits = validate_state_limits(snapshot.live);
  if (!live_limits.ok()) {
    return live_limits;
  }
  if (snapshot.live.revision < snapshot.checkpoint.revision) {
    return Status::error(StatusCode::StoreCorrupt, "live revision precedes the checkpoint");
  }

  auto reconstructed = replay(snapshot.checkpoint, snapshot.journal);
  if (!reconstructed.ok()) {
    return Status::error(StatusCode::ReplayDivergence,
                         "snapshot journal does not replay over its checkpoint")
        .with_context(reconstructed.status().to_string());
  }
  if (!states_encode_identically(reconstructed.value(), snapshot.live)) {
    return Status::error(StatusCode::ReplayDivergence,
                         "replayed state does not match the persisted live state");
  }
  return snapshot;
}

Result<std::unique_ptr<DurableStore>> DurableStore::Open(const StoreOpenOptions& options) {
  auto store = std::unique_ptr<DurableStore>(new DurableStore());
  store->options_ = options;

  // A volatile store owns no path, takes no lock, and writes nothing. Path
  // validation is therefore skipped entirely rather than applied to a directory
  // that will never be touched.
  if (options.durability == DurabilityMode::MemoryOnly) {
    store->options_.root = options.root;
    store->root_ = options.root;
    store->snapshot_ = StoreSnapshot{};
    store->snapshot_.store_generation = StoreGeneration::from_value(1);
    store->snapshot_.commit_sequence = CommitSequence::from_value(1);
    store->report_.created = true;
    store->report_.detail = "volatile store";
    store->report_.generation = store->snapshot_.store_generation;
    store->report_.commit = store->snapshot_.commit_sequence;
    return store;
  }

  auto root = canonicalize_store_root(options.root, options.create_if_missing);
  if (!root.ok()) {
    return root.status();
  }
  store->options_.root = root.value();
  store->root_ = root.value();

  {
    auto lock = StoreLock::Acquire(store->root_);
    if (!lock.ok()) {
      return lock.status();
    }
    store->lock_ = std::move(lock).value();
  }

  const std::uint64_t max_bytes = options.max_snapshot_bytes;
  SlotInfo slots[kSlotCount];
  bool any_present = false;
  for (std::uint32_t i = 0; i < kSlotCount; ++i) {
    slots[i] = inspect_slot(slot_path(store->root_, i), max_bytes);
    any_present = any_present || slots[i].present;
    if (slots[i].present && !slots[i].valid) {
      store->report_.other_slot_unusable = true;
    }
  }

  std::uint32_t chosen = 0;
  bool found = false;
  bool recovered_previous = false;
  for (std::uint32_t i = 0; i < kSlotCount; ++i) {
    if (!slots[i].valid) {
      continue;
    }
    if (!found || slots[chosen].commit < slots[i].commit ||
        (slots[chosen].commit == slots[i].commit &&
         slots[chosen].generation < slots[i].generation)) {
      if (found) {
        recovered_previous = true;
      }
      chosen = i;
      found = true;
    }
  }

  if (!found) {
    if (any_present) {
      std::string detail;
      for (std::uint32_t i = 0; i < kSlotCount; ++i) {
        if (slots[i].present) {
          detail.append("slot ");
          detail.append(std::to_string(i));
          detail.append(": ");
          detail.append(slots[i].status.to_string());
          detail.push_back(' ');
        }
      }
      return Status::error(StatusCode::StoreCorrupt,
                           "no slot holds a usable generation of the store")
          .with_context(detail);
    }
    if (!options.create_if_missing) {
      return Status::error(StatusCode::StoreNotFound, "store does not exist")
          .with_context(narrow(store->root_));
    }
    store->snapshot_ = StoreSnapshot{};
    store->snapshot_.store_generation = StoreGeneration::from_value(1);
    store->snapshot_.commit_sequence = CommitSequence::from_value(1);
    store->report_.created = true;
    store->authoritative_slot_ = 0;
    store->report_.generation = store->snapshot_.store_generation;
    store->report_.commit = store->snapshot_.commit_sequence;
    store->report_.detail = "created a new store";
    const Status created = store->Commit(Timestamp{});
    if (!created.ok()) {
      return created;
    }
    return store;
  }

  auto bytes = read_file(slot_path(store->root_, chosen), max_bytes + kSnapshotHeaderBytes +
                                                               kSnapshotTrailerBytes);
  if (!bytes.ok()) {
    return bytes.status();
  }
  auto snapshot = decode_snapshot(bytes.value(), max_bytes);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  store->snapshot_ = std::move(snapshot).value();
  store->authoritative_slot_ = chosen;
  store->report_.loaded = true;
  // Either a lower-sequence but valid generation was selected, or the other
  // slot was unusable and this store fell back to the previous complete
  // generation. Both are reported rather than hidden.
  store->report_.previous_generation_recovered = recovered_previous || store->report_.other_slot_unusable;
  store->report_.generation = store->snapshot_.store_generation;
  store->report_.commit = store->snapshot_.commit_sequence;
  store->report_.journal_entries = static_cast<std::uint32_t>(store->snapshot_.journal.size());
  store->report_.incidents = static_cast<std::uint32_t>(store->snapshot_.live.incidents.size());
  store->report_.zones = static_cast<std::uint32_t>(store->snapshot_.live.zones.size());
  store->report_.detail = recovered_previous ? "recovered the previous complete generation"
                                             : "loaded the latest complete generation";
  return store;
}

DurableStore::~DurableStore() { Close(); }

void DurableStore::Close() noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (lock_) {
    lock_->Release();
    lock_.reset();
  }
}

Status DurableStore::Commit(Timestamp committed_at) {
  if (closed_) {
    return Status::error(StatusCode::RuntimeClosed, "the store is closed");
  }
  snapshot_.commit_sequence = snapshot_.commit_sequence.is_set()
                                  ? snapshot_.commit_sequence.next()
                                  : CommitSequence::from_value(1);
  snapshot_.store_generation = snapshot_.store_generation.is_set()
                                   ? snapshot_.store_generation.next()
                                   : StoreGeneration::from_value(1);
  snapshot_.committed_at = committed_at;

  if (options_.durability == DurabilityMode::MemoryOnly) {
    ++commit_count_;
    return Status::success();
  }

  auto encoded = encode_snapshot(snapshot_, options_.max_snapshot_bytes);
  if (!encoded.ok()) {
    return encoded.status();
  }
  const std::vector<std::uint8_t>& bytes = encoded.value();

  const std::uint32_t target_slot = (authoritative_slot_ + 1u) % kSlotCount;
  const std::filesystem::path staged = stage_path(root_, target_slot);
  const std::filesystem::path destination = slot_path(root_, target_slot);

  Status written = write_file_durable(staged, bytes);
  if (!written.ok()) {
    remove_file(staged);
    return written;
  }
  auto read_back = read_file(staged, options_.max_snapshot_bytes + kSnapshotHeaderBytes +
                                         kSnapshotTrailerBytes);
  if (!read_back.ok()) {
    remove_file(staged);
    return read_back.status();
  }
  if (read_back.value() != bytes) {
    remove_file(staged);
    return Status::error(StatusCode::StoreReadbackMismatch,
                         "staged slot did not read back identically");
  }
  Status published = atomic_replace(staged, destination);
  if (!published.ok()) {
    remove_file(staged);
    return published;
  }
  sync_directory(root_);
  authoritative_slot_ = target_slot;
  ++commit_count_;
  return Status::success();
}

StoreVerification DurableStore::Verify() const {
  StoreVerification verification;
  const SlotInfo a = inspect_slot(slot_path(root_, 0), options_.max_snapshot_bytes);
  const SlotInfo b = inspect_slot(slot_path(root_, 1), options_.max_snapshot_bytes);
  verification.slot_a_present = a.present;
  verification.slot_b_present = b.present;
  verification.slot_a_valid = a.valid;
  verification.slot_b_valid = b.valid;
  verification.slot_a_commit = a.commit;
  verification.slot_b_commit = b.commit;
  verification.slot_a_generation = a.generation;
  verification.slot_b_generation = b.generation;
  verification.slot_a_bytes = a.bytes;
  verification.slot_b_bytes = b.bytes;
  verification.slot_a_payload_crc = a.payload_crc;
  verification.slot_b_payload_crc = b.payload_crc;
  verification.authoritative_slot = authoritative_slot_;
  if (options_.durability == DurabilityMode::MemoryOnly) {
    verification.ok = true;
    verification.detail = "volatile store: no durable slots exist";
    return verification;
  }
  verification.ok = (authoritative_slot_ == 0 ? a.valid : b.valid);
  const SlotInfo& authoritative = authoritative_slot_ == 0 ? a : b;
  const SlotInfo& other = authoritative_slot_ == 0 ? b : a;
  verification.authoritative_is_older = other.valid && authoritative.commit < other.commit;
  if (verification.ok) {
    verification.detail = "authoritative slot ";
    verification.detail.append(std::to_string(authoritative_slot_));
    verification.detail.append(" holds commit ");
    verification.detail.append(std::to_string(authoritative.commit.value()));
    if (!other.present) {
      verification.detail.append("; the other slot is not present");
    } else if (!other.valid) {
      verification.detail.append("; the other slot is unusable: ");
      verification.detail.append(other.status.to_string());
    }
  } else {
    verification.detail = "the authoritative slot is not usable: ";
    verification.detail.append(authoritative.status.to_string());
  }
  return verification;
}

}  // namespace summon::tem
