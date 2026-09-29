// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/codec.hpp"

#include <array>
#include <cstring>

namespace summon::tem::codec {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t i = 0; i < 256; ++i) {
    std::uint32_t crc = static_cast<std::uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (0x82F63B78u ^ (crc >> 1)) : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kCrcTable = make_crc_table();

constexpr std::uint64_t kFnvOffsetA = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvOffsetB = 0x9e3779b97f4a7c15ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

}  // namespace

std::uint32_t crc32c(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = seed ^ 0xFFFFFFFFu;
  for (const std::uint8_t byte : data) {
    crc = kCrcTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept { return crc32c(0u, data); }

Fingerprint fnv1a128(std::span<const std::uint8_t> data) noexcept {
  std::uint64_t high = kFnvOffsetA;
  std::uint64_t low = kFnvOffsetB ^ static_cast<std::uint64_t>(data.size());
  for (const std::uint8_t byte : data) {
    high ^= byte;
    high *= kFnvPrime;
    low ^= static_cast<std::uint64_t>(byte) + 0x9eull;
    low *= kFnvPrime;
  }
  return Fingerprint{high, low};
}

void Writer::fail(Status status) {
  if (ok_) {
    ok_ = false;
    error_ = std::move(status);
  }
}

void Writer::u8(std::uint8_t value) { bytes_.push_back(value); }

void Writer::u16(std::uint16_t value) {
  bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Writer::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Writer::i32(std::int32_t value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  u32(bits);
}

void Writer::i64(std::int64_t value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void Writer::flag(bool value) { u8(value ? 1u : 0u); }

void Writer::raw(std::span<const std::uint8_t> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }

bool Writer::text(const std::string& value, std::size_t max_len) {
  if (value.size() > max_len) {
    fail(Status::error(StatusCode::FieldTooLong, "text field exceeds the encodable bound"));
    return false;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  bytes_.insert(bytes_.end(), value.begin(), value.end());
  return true;
}

void Reader::fail(Status status) {
  if (ok_) {
    ok_ = false;
    error_ = std::move(status);
  }
}

bool Reader::need(std::size_t count) {
  if (!ok_) {
    return false;
  }
  if (count > data_.size() - position_) {
    fail(Status::error(StatusCode::StoreTruncated,
                       "encoded input ended before the declared field width"));
    return false;
  }
  return true;
}

std::uint8_t Reader::u8() {
  if (!need(1)) {
    return 0;
  }
  return data_[position_++];
}

std::uint16_t Reader::u16() {
  if (!need(2)) {
    return 0;
  }
  std::uint16_t value = 0;
  value |= static_cast<std::uint16_t>(data_[position_]);
  value |= static_cast<std::uint16_t>(data_[position_ + 1]) << 8;
  position_ += 2;
  return value;
}

std::uint32_t Reader::u32() {
  if (!need(4)) {
    return 0;
  }
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data_[position_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  position_ += 4;
  return value;
}

std::uint64_t Reader::u64() {
  if (!need(8)) {
    return 0;
  }
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data_[position_ + static_cast<std::size_t>(i)]) << (8 * i);
  }
  position_ += 8;
  return value;
}

std::int32_t Reader::i32() {
  const std::uint32_t bits = u32();
  std::int32_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::int64_t Reader::i64() {
  const std::uint64_t bits = u64();
  std::int64_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool Reader::flag() { return u8() != 0u; }

std::span<const std::uint8_t> Reader::raw(std::size_t count) {
  if (!need(count)) {
    return {};
  }
  const std::span<const std::uint8_t> result = data_.subspan(position_, count);
  position_ += count;
  return result;
}

bool Reader::text(std::string& out, std::size_t max_len) {
  const std::uint32_t length = u32();
  if (!ok_) {
    return false;
  }
  if (length > max_len) {
    fail(Status::error(StatusCode::FieldTooLong,
                       "declared text length exceeds the structural bound"));
    return false;
  }
  const std::span<const std::uint8_t> bytes = raw(length);
  if (!ok_) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

}  // namespace summon::tem::codec
