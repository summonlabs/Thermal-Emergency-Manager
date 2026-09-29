// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/obligations.hpp"

namespace summon::tem {

std::string_view protection_class_name(ProtectionClass value) noexcept {
  switch (value) {
    case ProtectionClass::AdvisoryOptimization: return "advisory-optimization";
    case ProtectionClass::Regulatory: return "regulatory";
    case ProtectionClass::HardSafetyInterlock: return "hard-safety-interlock";
  }
  return "unknown";
}

std::optional<ProtectionClass> protection_class_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return ProtectionClass::AdvisoryOptimization;
    case 2: return ProtectionClass::Regulatory;
    case 3: return ProtectionClass::HardSafetyInterlock;
    default: return std::nullopt;
  }
}

std::string_view obligation_status_name(ObligationStatus value) noexcept {
  switch (value) {
    case ObligationStatus::Satisfied: return "satisfied";
    case ObligationStatus::Violated: return "violated";
    case ObligationStatus::Unknown: return "unknown";
    case ObligationStatus::Unavailable: return "unavailable";
    case ObligationStatus::Unsupported: return "unsupported";
  }
  return "unknown";
}

std::optional<ObligationStatus> obligation_status_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return ObligationStatus::Satisfied;
    case 2: return ObligationStatus::Violated;
    case 3: return ObligationStatus::Unknown;
    case 4: return ObligationStatus::Unavailable;
    case 5: return ObligationStatus::Unsupported;
    default: return std::nullopt;
  }
}

bool obligation_covers(const ProtectedObligation& obligation, const RequestTarget& target) noexcept {
  if (!obligation.zone.valid()) {
    return true;  // domain-wide
  }
  if (target.kind() != RefKind::ThermalZone) {
    return false;
  }
  return target.zone() == obligation.zone;
}

bool relaxation_allowed(const RelaxationPolicy& policy, ProtectionClass protection,
                        MitigationClass cls) noexcept {
  if (protection != ProtectionClass::AdvisoryOptimization) {
    return false;  // regulatory obligations and hard interlocks are never relaxable here
  }
  if (!policy.allow_advisory_relaxation) {
    return false;
  }
  return (policy.relaxable_classes & mitigation_class_bit(cls)) != 0;
}

Status validate_obligation(const ProtectedObligation& obligation, const Bounds& bounds) {
  auto ref = canonical_ref_text(RefKind::ExternalAuthority, obligation.ref);
  if (!ref.ok()) {
    return ref.status();
  }
  if (obligation.description.size() > bounds.max_description_length) {
    return Status::error(StatusCode::FieldTooLong, "obligation description exceeds the bound")
        .with_context(obligation.ref);
  }
  const std::uint8_t known_mask = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
  if ((obligation.forbidden_classes & static_cast<std::uint8_t>(~known_mask)) != 0) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "obligation forbids a mitigation class outside the defined set")
        .with_context(obligation.ref);
  }
  if (obligation.forbidden_classes == 0) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "obligation must forbid at least one mitigation class")
        .with_context(obligation.ref);
  }
  if (obligation.protection != ProtectionClass::AdvisoryOptimization &&
      !obligation.authority.valid()) {
    return Status::error(StatusCode::InvalidArgument,
                         "regulatory and hard-safety obligations require an owning authority")
        .with_context(obligation.ref);
  }
  return Status::success();
}

}  // namespace summon::tem
