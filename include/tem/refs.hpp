// Thermal Emergency Manager -- opaque references to externally owned entities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "tem/status.hpp"

namespace summon::tem {

// The runtime never reimplements external identity. It carries opaque,
// canonically validated reference tokens whose meaning is owned by an adjacent
// authority (facility topology, power distribution, workload management,
// cooling plant, operational authority). A reference is an identity, not a
// path: the validation rules below exist so that two spellings can never name
// two different locks or two different entities.
enum class RefKind : std::uint8_t {
  ThermalZone = 1,      // facility thermal zone / cold aisle group
  ThermalProbe = 2,     // temperature probe or sensor aggregate within a zone
  Workload = 3,         // workload or workload group owned by a scheduling authority
  PowerDomain = 4,      // power distribution domain owned by the power authority
  Equipment = 5,        // isolatable equipment (valve, breaker, CDU branch)
  CoolingDomain = 6,    // cooling plant domain (used only as evidence source identity)
  Operator = 7,         // human or supervisory operator authority
  ExternalAuthority = 8,// owning authority that may change a protected obligation
  ExternalEvidence = 9, // opaque verification evidence handle from an external system
  SensorSource = 10,    // producer identity of a sample stream
};

[[nodiscard]] std::string_view ref_kind_name(RefKind kind) noexcept;
[[nodiscard]] std::optional<RefKind> ref_kind_from_value(std::uint8_t value) noexcept;

// Maximum length of a reference token, in bytes. Bounded so that persisted
// records and audit entries have a hard upper size.
inline constexpr std::size_t kMaxRefLength = 96;

// Validates reference text for a kind and returns the canonical form.
//
// Rules (checked in this order, first failure wins):
//   1. empty                       -> EmptyField
//   2. longer than kMaxRefLength   -> FieldTooLong
//   3. byte outside [A-Za-z0-9._:+-] or '/' -> InvalidCharacter
//   4. leading or trailing '/'     -> InvalidCharacter
//   5. "//" empty segment          -> InvalidCharacter
//   6. "." or ".." segment         -> InvalidCharacter
//
// The canonical form is the input text unchanged: accepted text is already
// canonical, so equality of tokens is byte equality and ordering is byte
// ordering. No case folding, no separator rewriting, no trimming is performed;
// such normalisation would make two different spellings compare equal and is
// therefore refused rather than applied.
[[nodiscard]] Result<std::string> canonical_ref_text(RefKind kind, std::string_view text);

// Untyped reference token: kind plus canonical text.
class RefToken {
 public:
  RefToken() noexcept = default;
  RefToken(RefKind kind, std::string text) : kind_(kind), text_(std::move(text)) {}

  [[nodiscard]] static Result<RefToken> parse(RefKind kind, std::string_view text);

  [[nodiscard]] bool valid() const noexcept { return !text_.empty(); }
  [[nodiscard]] RefKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string& text() const noexcept { return text_; }

  [[nodiscard]] friend bool operator==(const RefToken& a, const RefToken& b) noexcept {
    return a.kind_ == b.kind_ && a.text_ == b.text_;
  }
  [[nodiscard]] friend bool operator<(const RefToken& a, const RefToken& b) noexcept {
    if (a.kind_ != b.kind_) {
      return static_cast<std::uint8_t>(a.kind_) < static_cast<std::uint8_t>(b.kind_);
    }
    return a.text_ < b.text_;
  }

 private:
  RefKind kind_{RefKind::ThermalZone};
  std::string text_{};
};

// Typed reference: the kind is part of the type, so a power-domain reference
// can never be passed where a thermal-zone reference is expected.
template <RefKind Kind>
class Ref {
 public:
  using kind_tag = std::integral_constant<RefKind, Kind>;

  // An absent reference still carries its kind, so an empty typed reference is
  // never mistaken for a reference of another kind.
  Ref() noexcept : token_(Kind, std::string()) {}
  explicit Ref(std::string canonical_text) : token_(Kind, std::move(canonical_text)) {}

  [[nodiscard]] static Result<Ref> parse(std::string_view text) {
    auto token = RefToken::parse(Kind, text);
    if (!token.ok()) {
      return token.status();
    }
    return Ref{std::move(token).value().text()};
  }

  [[nodiscard]] static constexpr RefKind kind() noexcept { return Kind; }
  [[nodiscard]] bool valid() const noexcept { return token_.valid(); }
  [[nodiscard]] const std::string& text() const noexcept { return token_.text(); }
  [[nodiscard]] const RefToken& token() const noexcept { return token_; }

  [[nodiscard]] friend bool operator==(const Ref& a, const Ref& b) noexcept = default;
  [[nodiscard]] friend auto operator<=>(const Ref& a, const Ref& b) noexcept = default;

 private:
  RefToken token_{};
};

using ThermalZoneRef = Ref<RefKind::ThermalZone>;
using ThermalProbeRef = Ref<RefKind::ThermalProbe>;
using WorkloadRef = Ref<RefKind::Workload>;
using PowerDomainRef = Ref<RefKind::PowerDomain>;
using EquipmentRef = Ref<RefKind::Equipment>;
using CoolingDomainRef = Ref<RefKind::CoolingDomain>;
using OperatorRef = Ref<RefKind::Operator>;
using ExternalAuthorityRef = Ref<RefKind::ExternalAuthority>;
using ExternalEvidenceRef = Ref<RefKind::ExternalEvidence>;
using SensorSourceRef = Ref<RefKind::SensorSource>;

}  // namespace summon::tem
