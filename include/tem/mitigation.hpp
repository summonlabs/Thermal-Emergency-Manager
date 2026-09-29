// Thermal Emergency Manager -- bounded cross-domain mitigation requests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tem/ids.hpp"
#include "tem/refs.hpp"
#include "tem/severity.hpp"
#include "tem/status.hpp"
#include "tem/units.hpp"

namespace summon::tem {

// The four bounded mitigation classes this runtime may request. Each request
// asks an adjacent authority to act; this runtime never actuates anything
// itself and never claims the effect of a request it merely issued.
enum class MitigationClass : std::uint8_t {
  DerateAccelerators = 1,  // ask the accelerator authority to cap compute in a thermal zone
  DrainWorkload = 2,       // ask the workload authority to drain a thermal zone
  ReducePower = 3,         // ask the power authority to reduce draw in a power domain
  IsolateEquipment = 4,    // ask the facility authority to isolate equipment
};

inline constexpr std::uint8_t kMitigationClassCount = 4;

[[nodiscard]] std::string_view mitigation_class_name(MitigationClass value) noexcept;
[[nodiscard]] std::optional<MitigationClass> mitigation_class_from_value(std::uint8_t value) noexcept;
[[nodiscard]] constexpr std::uint8_t mitigation_class_ordinal(MitigationClass value) noexcept {
  return static_cast<std::uint8_t>(value);
}
[[nodiscard]] constexpr std::uint8_t mitigation_class_bit(MitigationClass value) noexcept {
  return static_cast<std::uint8_t>(1u << (mitigation_class_ordinal(value) - 1u));
}
// Reference kind required by each class. A request whose target does not match
// its class is refused with RequestTargetMismatch.
[[nodiscard]] RefKind mitigation_target_kind(MitigationClass value) noexcept;

// The classes required at a given severity, in deterministic evaluation order
// (class ordinal ascending). Returns a bitmask over mitigation_class_bit().
[[nodiscard]] constexpr std::uint8_t required_class_mask(Severity severity) noexcept {
  switch (severity) {
    case Severity::Nominal:
    case Severity::Advisory:
      return 0;
    case Severity::Warning:
      return 1u << 0;  // DerateAccelerators
    case Severity::Critical:
      return (1u << 0) | (1u << 1);  // + DrainWorkload
    case Severity::Emergency:
      return (1u << 0) | (1u << 1) | (1u << 2);  // + ReducePower
    case Severity::Catastrophic:
      return (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);  // + IsolateEquipment
  }
  return 0;
}

// Request targets are strongly typed: a thermal-zone request can never carry a
// power-domain reference.
class RequestTarget {
 public:
  RequestTarget() noexcept = default;

  [[nodiscard]] static RequestTarget for_zone(ThermalZoneRef zone) {
    return RequestTarget{std::move(zone)};
  }
  [[nodiscard]] static RequestTarget for_power_domain(PowerDomainRef domain) {
    return RequestTarget{std::move(domain)};
  }
  [[nodiscard]] static RequestTarget for_equipment(EquipmentRef equipment) {
    return RequestTarget{std::move(equipment)};
  }

  [[nodiscard]] RefKind kind() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const RefToken& token() const noexcept;

  // Precondition: kind() matches. Violations trap.
  [[nodiscard]] const ThermalZoneRef& zone() const;
  [[nodiscard]] const PowerDomainRef& power_domain() const;
  [[nodiscard]] const EquipmentRef& equipment() const;

  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] friend bool operator==(const RequestTarget& a, const RequestTarget& b) noexcept {
    return a.token() == b.token();
  }
  [[nodiscard]] friend bool operator<(const RequestTarget& a, const RequestTarget& b) noexcept {
    return a.token() < b.token();
  }

 private:
  using Storage = std::variant<ThermalZoneRef, PowerDomainRef, EquipmentRef>;
  explicit RequestTarget(ThermalZoneRef zone) : value_(std::move(zone)) {}
  explicit RequestTarget(PowerDomainRef domain) : value_(std::move(domain)) {}
  explicit RequestTarget(EquipmentRef equipment) : value_(std::move(equipment)) {}

  Storage value_{ThermalZoneRef{}};
};

// The distinct lifecycle of a mitigation request. An acknowledgement is not an
// observation, and an observation is not a verification.
enum class MitigationState : std::uint8_t {
  Planned = 1,       // persisted intent, not yet dispatched
  Issued = 2,        // dispatched to the adjacent authority, outcome unknown
  Acknowledged = 3,  // the adjacent authority accepted the request
  Observed = 4,      // an external observation sequence reports the request in effect
  Verified = 5,      // independent external evidence confirms the requested effect
  Failed = 6,        // terminal: refused, errored, timed out, or indeterminate
  Superseded = 7,    // terminal: replaced by a newer attempt or fenced by rollover
  Abandoned = 8,     // terminal: no longer required and never verified
  Expired = 9,       // terminal: validity window elapsed before acknowledgement
  Refused = 10,      // terminal: refused before dispatch by policy or obligation
};

inline constexpr std::uint8_t kMitigationStateCount = 10;

[[nodiscard]] std::string_view mitigation_state_name(MitigationState value) noexcept;
[[nodiscard]] std::optional<MitigationState> mitigation_state_from_value(std::uint8_t value) noexcept;

// Terminal states never change again. Verified is terminal *and* satisfying;
// Failed/Superseded/Abandoned/Expired/Refused are terminal and unsatisfying.
[[nodiscard]] constexpr bool mitigation_state_is_terminal(MitigationState value) noexcept {
  return value == MitigationState::Verified || value == MitigationState::Failed ||
         value == MitigationState::Superseded || value == MitigationState::Abandoned ||
         value == MitigationState::Expired || value == MitigationState::Refused;
}
[[nodiscard]] constexpr bool mitigation_state_is_open(MitigationState value) noexcept {
  return !mitigation_state_is_terminal(value);
}
// True when the request has been dispatched to an external authority (so a
// retry must not dispatch again without a new attempt identity).
[[nodiscard]] constexpr bool mitigation_state_is_dispatched(MitigationState value) noexcept {
  return value == MitigationState::Issued || value == MitigationState::Acknowledged ||
         value == MitigationState::Observed;
}
// True when the state counts as satisfying a required mitigation class.
[[nodiscard]] constexpr bool mitigation_state_satisfies(MitigationState value) noexcept {
  return value == MitigationState::Verified;
}

// Why the request was raised.
enum class RequestReason : std::uint8_t {
  EscalationSeverity = 1,
  EvidenceRate = 2,
  EvidenceLoss = 3,
  ReissueAfterFailure = 4,
  OperatorRequest = 5,
  ObligationRelief = 6,
};

[[nodiscard]] std::string_view request_reason_name(RequestReason value) noexcept;
[[nodiscard]] std::optional<RequestReason> request_reason_from_value(std::uint8_t value) noexcept;

// Why a request reached a terminal non-verified state.
enum class FailureCode : std::uint8_t {
  None = 0,
  TransportRefused = 1,
  TransportError = 2,
  VerificationTimeout = 3,
  ExpiryElapsed = 4,
  ExternalFailure = 5,
  IndeterminateDispatch = 6,
  ProtectedObligation = 7,
  Unsupported = 8,
};

[[nodiscard]] std::string_view failure_code_name(FailureCode value) noexcept;
[[nodiscard]] std::optional<FailureCode> failure_code_from_value(std::uint8_t value) noexcept;

enum class SupersedeReason : std::uint8_t {
  None = 0,
  AuthorityRollover = 1,
  SeverityReduced = 2,
  ReplacedByNewerAttempt = 3,
  PlanChanged = 4,
};

[[nodiscard]] std::string_view supersede_reason_name(SupersedeReason value) noexcept;
[[nodiscard]] std::optional<SupersedeReason> supersede_reason_from_value(std::uint8_t value) noexcept;

// A request as recorded. Every field required to attribute, order, deduplicate,
// and expire the request is present: identity, incident, generation, epoch,
// incarnation, the revision it was planned against, the target, the requested
// intensity, evidence references, and the idempotency identity.
struct MitigationRequest {
  MitigationRequestId id{};
  AttemptId attempt{};
  CommandId command{};
  IncidentId incident{};
  IncidentGeneration incident_generation{};
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision planned_revision{};

  MitigationClass cls{MitigationClass::DerateAccelerators};
  RequestTarget target{};
  BasisPoints intensity{};
  RequestReason reason{RequestReason::EscalationSeverity};

  std::vector<RefToken> evidence_refs{};  // bounded by Bounds::max_evidence_refs

  Fingerprint fingerprint{};
  IdempotencyKey key{};

  Timestamp created_at{};
  Timestamp expires_at{};
  bool has_expiry{true};
  Timestamp verification_deadline{};
  bool has_verification_deadline{true};

  [[nodiscard]] bool has_expired_at(Timestamp now) const noexcept {
    return has_expiry && now > expires_at;
  }
  [[nodiscard]] bool verification_overdue_at(Timestamp now) const noexcept {
    return has_verification_deadline && now > verification_deadline;
  }
};

// Canonical semantic fingerprint: incident identity, incident generation,
// control epoch, class, target, and requested intensity. Attempt identity,
// command identity, revision, evidence references, and timestamps are excluded
// on purpose: a lost-response retry of the same semantics must produce the same
// fingerprint, while a request issued under a new authority epoch must not.
[[nodiscard]] Fingerprint fingerprint_of_request_fields(IncidentId incident,
                                                        IncidentGeneration generation,
                                                        ControlEpoch epoch,
                                                        MitigationClass cls,
                                                        const RequestTarget& target,
                                                        BasisPoints intensity) noexcept;

[[nodiscard]] inline Fingerprint fingerprint_of(const MitigationRequest& request) noexcept {
  return fingerprint_of_request_fields(request.incident, request.incident_generation, request.epoch,
                                       request.cls, request.target, request.intensity);
}

// Deterministic ordering key: priority (class ordinal), then target bytes, then
// request id. Used to order issuance and dispatch so that two runs over the
// same evidence produce byte-identical action sequences.
struct RequestOrderKey {
  std::uint8_t priority{0};
  RefToken target{};
  MitigationRequestId id{};

  [[nodiscard]] friend bool operator<(const RequestOrderKey& a, const RequestOrderKey& b) noexcept {
    if (a.priority != b.priority) {
      return a.priority < b.priority;
    }
    if (!(a.target == b.target)) {
      return a.target < b.target;
    }
    return a.id < b.id;
  }
};

[[nodiscard]] RequestOrderKey order_key_of(const MitigationRequest& request) noexcept;

// A recorded state change of a request.
struct RequestHistoryPoint {
  MitigationState state{MitigationState::Planned};
  Timestamp at{};
  FailureCode failure{FailureCode::None};
  SupersedeReason supersede{SupersedeReason::None};
  std::int32_t detail{0};  // bounded numeric detail (e.g. transport code)
};

// A request plus its live state and bounded history.
struct MitigationRecord {
  MitigationRequest request{};
  MitigationState state{MitigationState::Planned};
  Timestamp state_at{};
  FailureCode failure{FailureCode::None};
  SupersedeReason supersede{SupersedeReason::None};
  ExternalEvidenceRef external_ref{};
  bool has_external_ref{false};
  ObservationSequence last_observation{};
  Timestamp verified_at{};
  bool has_verified_at{false};
  ExternalEvidenceRef verification_ref{};
  bool has_verification_ref{false};
  std::vector<RequestHistoryPoint> history{};
  std::int32_t transport_code{0};
  bool recovered_from_store{false};

  [[nodiscard]] bool is_terminal() const noexcept { return mitigation_state_is_terminal(state); }
  [[nodiscard]] bool is_open() const noexcept { return mitigation_state_is_open(state); }
};

}  // namespace summon::tem
