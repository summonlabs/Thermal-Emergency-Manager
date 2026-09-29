// Thermal Emergency Manager -- canonical binary encoding primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "tem/evidence.hpp"
#include "tem/ids.hpp"
#include "tem/journal.hpp"
#include "tem/mitigation.hpp"
#include "tem/obligations.hpp"
#include "tem/policy.hpp"
#include "tem/recovery.hpp"
#include "tem/refs.hpp"
#include "tem/severity.hpp"
#include "tem/state.hpp"
#include "tem/status.hpp"
#include "tem/units.hpp"

namespace summon::tem::codec {

// CRC-32C (Castagnoli), reflected, init 0xFFFFFFFF, final xor 0xFFFFFFFF.
// Used for record and whole-payload integrity.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept;

// 128-bit FNV-1a over canonical bytes. Used for semantic fingerprints and
// idempotency keys. Chosen for stability and simplicity, not for adversarial
// collision resistance; keys are also compared against the recorded
// fingerprint, so a collision cannot silently satisfy a different request.
[[nodiscard]] Fingerprint fnv1a128(std::span<const std::uint8_t> data) noexcept;

// Canonical writer. All integers are little-endian, all lengths are explicit
// 32-bit counts, and no padding is ever emitted. Every variable-length field is
// bounded by the caller before it is written.
class Writer {
 public:
  Writer() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i32(std::int32_t value);
  void i64(std::int64_t value);
  void flag(bool value);
  void raw(std::span<const std::uint8_t> data);
  // Length-prefixed text. Returns false and latches an error when the text is
  // longer than max_len, so an oversized field can never be encoded. Callers
  // check ok() once at the end of an encode path.
  bool text(const std::string& value, std::size_t max_len);
  void count(std::uint32_t value) { u32(value); }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Status& error() const noexcept { return error_; }

  // Used by nested encoders that must refuse to write past a bound.
  void fail(Status status);

 private:
  std::vector<std::uint8_t> bytes_{};
  bool ok_{true};
  Status error_{};
};

// Strict reader. Every read checks bounds; the first failure is latched and
// later reads become no-ops, so a decoder can be written linearly and validated
// once at the end. Trailing bytes are detected with at_end().
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

  [[nodiscard]] std::uint8_t u8();
  [[nodiscard]] std::uint16_t u16();
  [[nodiscard]] std::uint32_t u32();
  [[nodiscard]] std::uint64_t u64();
  [[nodiscard]] std::int32_t i32();
  [[nodiscard]] std::int64_t i64();
  [[nodiscard]] bool flag();

  // Reads n bytes. Returns an empty span and latches an error when the request
  // would run past the end of the buffer.
  [[nodiscard]] std::span<const std::uint8_t> raw(std::size_t n);

  // Reads length-prefixed text. max_len is the hard structural bound; a
  // declared length above it is refused *before* any allocation.
  bool text(std::string& out, std::size_t max_len);

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Status& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
  [[nodiscard]] bool at_end() const noexcept { return position_ == data_.size(); }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }

  void fail(Status status);

 private:
  [[nodiscard]] bool need(std::size_t n);

  std::span<const std::uint8_t> data_{};
  std::size_t position_{0};
  bool ok_{true};
  Status error_{};
};

}  // namespace summon::tem::codec

namespace summon::tem {

// Domain encoders and decoders. Decoding latches the first failure on the
// reader; callers check reader.ok() and then reader.at_end().
namespace codec {

void encode(Writer& writer, const RefToken& value);
void decode(Reader& reader, RefToken& value);

void encode(Writer& writer, Timestamp value);
void decode(Reader& reader, Timestamp& value);
void encode(Writer& writer, Duration value);
void decode(Reader& reader, Duration& value);

void encode(Writer& writer, MilliCelsius value);
void decode(Reader& reader, MilliCelsius& value);
void encode(Writer& writer, MilliCelsiusPerMinute value);
void decode(Reader& reader, MilliCelsiusPerMinute& value);
void encode(Writer& writer, BasisPoints value);
void decode(Reader& reader, BasisPoints& value);

void encode(Writer& writer, Fingerprint value);
void decode(Reader& reader, Fingerprint& value);
void encode(Writer& writer, IdempotencyKey value);
void decode(Reader& reader, IdempotencyKey& value);

void encode(Writer& writer, Severity value);
void decode(Reader& reader, Severity& value);
void encode(Writer& writer, Lifecycle value);
void decode(Reader& reader, Lifecycle& value);
void encode(Writer& writer, Disposition value);
void decode(Reader& reader, Disposition& value);
void encode(Writer& writer, MitigationClass value);
void decode(Reader& reader, MitigationClass& value);
void encode(Writer& writer, MitigationState value);
void decode(Reader& reader, MitigationState& value);
void encode(Writer& writer, RequestReason value);
void decode(Reader& reader, RequestReason& value);
void encode(Writer& writer, FailureCode value);
void decode(Reader& reader, FailureCode& value);
void encode(Writer& writer, SupersedeReason value);
void decode(Reader& reader, SupersedeReason& value);
void encode(Writer& writer, SampleQuality value);
void decode(Reader& reader, SampleQuality& value);
void encode(Writer& writer, SensorHealth value);
void decode(Reader& reader, SensorHealth& value);
void encode(Writer& writer, SampleOrigin value);
void decode(Reader& reader, SampleOrigin& value);
void encode(Writer& writer, ProtectionClass value);
void decode(Reader& reader, ProtectionClass& value);
void encode(Writer& writer, ObligationStatus value);
void decode(Reader& reader, ObligationStatus& value);
void encode(Writer& writer, TransitionTrigger value);
void decode(Reader& reader, TransitionTrigger& value);
void encode(Writer& writer, JournalKind value);
void decode(Reader& reader, JournalKind& value);

void encode(Writer& writer, const RequestTarget& value);
void decode(Reader& reader, RequestTarget& value);
void encode(Writer& writer, const ThermalSample& value);
void decode(Reader& reader, ThermalSample& value);
void encode(Writer& writer, const MitigationRequest& value);
void decode(Reader& reader, MitigationRequest& value);
void encode(Writer& writer, const RequestHistoryPoint& value);
void decode(Reader& reader, RequestHistoryPoint& value);
void encode(Writer& writer, const MitigationRecord& value);
void decode(Reader& reader, MitigationRecord& value);
void encode(Writer& writer, const ProtectedObligation& value);
void decode(Reader& reader, ProtectedObligation& value);
void encode(Writer& writer, const RelaxationRecord& value);
void decode(Reader& reader, RelaxationRecord& value);
void encode(Writer& writer, const TransitionRecord& value);
void decode(Reader& reader, TransitionRecord& value);
void encode(Writer& writer, const ProbeSlot& value);
void decode(Reader& reader, ProbeSlot& value);
void encode(Writer& writer, const ZoneSlot& value);
void decode(Reader& reader, ZoneSlot& value);
void encode(Writer& writer, const ObligationSlot& value);
void decode(Reader& reader, ObligationSlot& value);
void encode(Writer& writer, const IncidentProjection& value);
void decode(Reader& reader, IncidentProjection& value);
void encode(Writer& writer, const ThermalPolicy& value);
void decode(Reader& reader, ThermalPolicy& value);
void encode(Writer& writer, const RelaxationPolicy& value);
void decode(Reader& reader, RelaxationPolicy& value);
void encode(Writer& writer, const Bounds& value);
void decode(Reader& reader, Bounds& value);
void encode(Writer& writer, const DomainState& value);
void decode(Reader& reader, DomainState& value);
void encode(Writer& writer, const JournalPayload& value);
void decode(Reader& reader, JournalPayload& value);
void encode(Writer& writer, const JournalEntry& value);
void decode(Reader& reader, JournalEntry& value);

}  // namespace codec

// Canonical byte encoding of a domain state, used for byte-for-byte determinism
// checks and for encoding equality.
[[nodiscard]] std::vector<std::uint8_t> encode_state(const DomainState& state);

}  // namespace summon::tem
