// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/ids.hpp"

#include <cstdint>
#include <string>

namespace summon::tem {
namespace {

std::string hex128(std::uint64_t high, std::uint64_t low) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(32);
  for (int i = 15; i >= 0; --i) {
    const std::uint64_t nibble = (high >> (i * 4)) & 0xFull;
    out[static_cast<std::size_t>(15 - i)] = kDigits[nibble];
  }
  for (int i = 15; i >= 0; --i) {
    const std::uint64_t nibble = (low >> (i * 4)) & 0xFull;
    out[static_cast<std::size_t>(31 - i)] = kDigits[nibble];
  }
  return out;
}

}  // namespace

std::string Fingerprint::to_hex() const { return hex128(high_, low_); }
std::string IdempotencyKey::to_hex() const { return hex128(high_, low_); }

}  // namespace summon::tem
