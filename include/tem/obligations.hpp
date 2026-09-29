// Thermal Emergency Manager -- protected obligations that constrain mitigation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "tem/ids.hpp"
#include "tem/mitigation.hpp"
#include "tem/policy.hpp"
#include "tem/refs.hpp"
#include "tem/status.hpp"
#include "tem/units.hpp"

namespace summon::tem {

// Protection classes decide what emergency authority may do. The emergency
// authority may relax advisory optimisation constraints only, and only where a
// relaxation policy explicitly allows it. It can never relax a regulatory
// obligation or a hard safety interlock, and it can never fabricate the
// external authority that owns them.
enum class ProtectionClass : std::uint8_t {
  AdvisoryOptimization = 1,
  Regulatory = 2,
  HardSafetyInterlock = 3,
};

[[nodiscard]] std::string_view protection_class_name(ProtectionClass value) noexcept;
[[nodiscard]] std::optional<ProtectionClass> protection_class_from_value(std::uint8_t value) noexcept;

// The live status of an obligation as reported by the authority that owns it.
enum class ObligationStatus : std::uint8_t {
  Satisfied = 1,    // the owning authority asserts the obligation holds
  Violated = 2,     // the owning authority asserts the obligation is breached
  Unknown = 3,      // status has never been reported, or the report expired
  Unavailable = 4,  // the owning authority is currently unreachable
  Unsupported = 5,  // the owning authority cannot express this obligation
};

[[nodiscard]] std::string_view obligation_status_name(ObligationStatus value) noexcept;
[[nodiscard]] std::optional<ObligationStatus> obligation_status_from_value(std::uint8_t value) noexcept;

struct ProtectedObligation {
  ObligationId id{};
  std::string ref{};                  // opaque reference owned by the external authority
  ThermalZoneRef zone{};              // empty zone means domain-wide
  std::string description{};
  ProtectionClass protection{ProtectionClass::HardSafetyInterlock};
  std::uint8_t forbidden_classes{0};  // bitmask over mitigation_class_bit()
  ExternalAuthorityRef authority{};   // who owns and may change the obligation

  ObligationStatus status{ObligationStatus::Unknown};
  Timestamp status_at{};
  bool status_present{false};
  ExternalAuthorityRef status_source{};
};

// True when the obligation forbids the class.
[[nodiscard]] constexpr bool obligation_forbids(const ProtectedObligation& obligation,
                                                MitigationClass cls) noexcept {
  return (obligation.forbidden_classes & mitigation_class_bit(cls)) != 0;
}

// True when the obligation scope covers the request target. Zone-scoped
// obligations cover zone targets; a domain-wide obligation covers everything.
[[nodiscard]] bool obligation_covers(const ProtectedObligation& obligation,
                                     const RequestTarget& target) noexcept;

// The constraints the emergency authority may relax. Only advisory
// optimisation constraints are ever eligible.
struct RelaxationPolicy {
  PolicyGeneration generation{};
  bool allow_advisory_relaxation{true};
  std::uint8_t relaxable_classes{0};  // bitmask over mitigation_class_bit()
};

[[nodiscard]] bool relaxation_allowed(const RelaxationPolicy& policy,
                                      ProtectionClass protection,
                                      MitigationClass cls) noexcept;

// Validation precedence for obligation registration:
//   1. reference token malformed                 -> InvalidArgument family
//   2. description longer than the bound         -> FieldTooLong
//   3. forbidden_classes has bits outside the
//      four defined classes                      -> ValueOutOfRange
//   4. forbidden_classes empty                   -> ValueOutOfRange
//   5. Regulatory or Hard class with an empty
//      owning authority                          -> InvalidArgument
[[nodiscard]] Status validate_obligation(const ProtectedObligation& obligation, const Bounds& bounds);

// A currently active relaxation. Relaxations are automatically re-imposed when
// the incident that granted them closes.
struct RelaxationRecord {
  ObligationId obligation{};
  MitigationClass cls{MitigationClass::DerateAccelerators};
  ExternalAuthorityRef granted_by{};
  Timestamp granted_at{};
  bool active{true};
};

}  // namespace summon::tem
