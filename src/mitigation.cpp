// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/mitigation.hpp"

#include <string>

#include "tem/codec.hpp"

namespace summon::tem {

std::string_view mitigation_class_name(MitigationClass value) noexcept {
  switch (value) {
    case MitigationClass::DerateAccelerators: return "derate-accelerators";
    case MitigationClass::DrainWorkload: return "drain-workload";
    case MitigationClass::ReducePower: return "reduce-power";
    case MitigationClass::IsolateEquipment: return "isolate-equipment";
  }
  return "unknown";
}

std::optional<MitigationClass> mitigation_class_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return MitigationClass::DerateAccelerators;
    case 2: return MitigationClass::DrainWorkload;
    case 3: return MitigationClass::ReducePower;
    case 4: return MitigationClass::IsolateEquipment;
    default: return std::nullopt;
  }
}

RefKind mitigation_target_kind(MitigationClass value) noexcept {
  switch (value) {
    case MitigationClass::DerateAccelerators:
    case MitigationClass::DrainWorkload:
      return RefKind::ThermalZone;
    case MitigationClass::ReducePower:
      return RefKind::PowerDomain;
    case MitigationClass::IsolateEquipment:
      return RefKind::Equipment;
  }
  return RefKind::ThermalZone;
}

RefKind RequestTarget::kind() const noexcept {
  if (std::holds_alternative<PowerDomainRef>(value_)) {
    return RefKind::PowerDomain;
  }
  if (std::holds_alternative<EquipmentRef>(value_)) {
    return RefKind::Equipment;
  }
  return RefKind::ThermalZone;
}

bool RequestTarget::valid() const noexcept { return token().valid(); }

const RefToken& RequestTarget::token() const noexcept {
  return std::visit([](const auto& ref) -> const RefToken& { return ref.token(); }, value_);
}

const ThermalZoneRef& RequestTarget::zone() const {
  if (!std::holds_alternative<ThermalZoneRef>(value_)) {
    TEM_TRAP("RequestTarget::zone() on a non-zone target");
  }
  return std::get<ThermalZoneRef>(value_);
}

const PowerDomainRef& RequestTarget::power_domain() const {
  if (!std::holds_alternative<PowerDomainRef>(value_)) {
    TEM_TRAP("RequestTarget::power_domain() on a non-domain target");
  }
  return std::get<PowerDomainRef>(value_);
}

const EquipmentRef& RequestTarget::equipment() const {
  if (!std::holds_alternative<EquipmentRef>(value_)) {
    TEM_TRAP("RequestTarget::equipment() on a non-equipment target");
  }
  return std::get<EquipmentRef>(value_);
}

std::string RequestTarget::to_string() const {
  std::string out(ref_kind_name(kind()));
  out.push_back(':');
  out.append(token().text());
  return out;
}

std::string_view mitigation_state_name(MitigationState value) noexcept {
  switch (value) {
    case MitigationState::Planned: return "planned";
    case MitigationState::Issued: return "issued";
    case MitigationState::Acknowledged: return "acknowledged";
    case MitigationState::Observed: return "observed";
    case MitigationState::Verified: return "verified";
    case MitigationState::Failed: return "failed";
    case MitigationState::Superseded: return "superseded";
    case MitigationState::Abandoned: return "abandoned";
    case MitigationState::Expired: return "expired";
    case MitigationState::Refused: return "refused";
  }
  return "unknown";
}

std::optional<MitigationState> mitigation_state_from_value(std::uint8_t value) noexcept {
  if (value < 1 || value > kMitigationStateCount) {
    return std::nullopt;
  }
  return static_cast<MitigationState>(value);
}

std::string_view request_reason_name(RequestReason value) noexcept {
  switch (value) {
    case RequestReason::EscalationSeverity: return "escalation-severity";
    case RequestReason::EvidenceRate: return "evidence-rate";
    case RequestReason::EvidenceLoss: return "evidence-loss";
    case RequestReason::ReissueAfterFailure: return "reissue-after-failure";
    case RequestReason::OperatorRequest: return "operator-request";
    case RequestReason::ObligationRelief: return "obligation-relief";
  }
  return "unknown";
}

std::optional<RequestReason> request_reason_from_value(std::uint8_t value) noexcept {
  if (value < 1 || value > 6) {
    return std::nullopt;
  }
  return static_cast<RequestReason>(value);
}

std::string_view failure_code_name(FailureCode value) noexcept {
  switch (value) {
    case FailureCode::None: return "none";
    case FailureCode::TransportRefused: return "transport-refused";
    case FailureCode::TransportError: return "transport-error";
    case FailureCode::VerificationTimeout: return "verification-timeout";
    case FailureCode::ExpiryElapsed: return "expiry-elapsed";
    case FailureCode::ExternalFailure: return "external-failure";
    case FailureCode::IndeterminateDispatch: return "indeterminate-dispatch";
    case FailureCode::ProtectedObligation: return "protected-obligation";
    case FailureCode::Unsupported: return "unsupported";
  }
  return "unknown";
}

std::optional<FailureCode> failure_code_from_value(std::uint8_t value) noexcept {
  if (value > 8) {
    return std::nullopt;
  }
  return static_cast<FailureCode>(value);
}

std::string_view supersede_reason_name(SupersedeReason value) noexcept {
  switch (value) {
    case SupersedeReason::None: return "none";
    case SupersedeReason::AuthorityRollover: return "authority-rollover";
    case SupersedeReason::SeverityReduced: return "severity-reduced";
    case SupersedeReason::ReplacedByNewerAttempt: return "replaced-by-newer-attempt";
    case SupersedeReason::PlanChanged: return "plan-changed";
  }
  return "unknown";
}

std::optional<SupersedeReason> supersede_reason_from_value(std::uint8_t value) noexcept {
  if (value > 4) {
    return std::nullopt;
  }
  return static_cast<SupersedeReason>(value);
}

Fingerprint fingerprint_of_request_fields(IncidentId incident, IncidentGeneration generation,
                                          ControlEpoch epoch, MitigationClass cls,
                                          const RequestTarget& target,
                                          BasisPoints intensity) noexcept {
  codec::Writer writer;
  writer.u64(incident.value());
  writer.u64(generation.value());
  writer.u64(epoch.value());
  writer.u8(mitigation_class_ordinal(cls));
  writer.u8(static_cast<std::uint8_t>(target.kind()));
  const std::string& text = target.token().text();
  writer.raw(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  writer.u16(intensity.value());
  return codec::fnv1a128(writer.bytes());
}

RequestOrderKey order_key_of(const MitigationRequest& request) noexcept {
  RequestOrderKey key;
  key.priority = mitigation_class_ordinal(request.cls);
  key.target = request.target.token();
  key.id = request.id;
  return key;
}

}  // namespace summon::tem
