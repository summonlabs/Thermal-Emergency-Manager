// Thermal Emergency Manager -- durable journal of authoritative inputs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tem/evidence.hpp"
#include "tem/ids.hpp"
#include "tem/mitigation.hpp"
#include "tem/obligations.hpp"
#include "tem/recovery.hpp"
#include "tem/severity.hpp"
#include "tem/units.hpp"

namespace summon::tem {

// The journal records the authoritative *inputs* the runtime accepted, never
// derived conclusions. Severity, lifecycle, gate results, and request
// aggregates are recomputed from this input history, which is what makes
// replay a real check rather than a copy of the answer.
//
// Numeric values are part of the persisted format and never change meaning.
enum class JournalKind : std::uint8_t {
  IncidentOpened = 1,
  ZoneRegistered = 2,
  ProbeRegistered = 3,
  ObligationRegistered = 4,
  EvidenceAdmitted = 5,
  ObligationStatusReported = 6,
  Tick = 7,
  RequestsIssued = 8,
  RequestAcknowledged = 9,
  RequestObserved = 10,
  RequestVerified = 11,
  RequestFailed = 12,
  RequestSuperseded = 13,
  RequestAbandoned = 14,
  ConstraintRelaxed = 15,
  ConstraintReimposed = 16,
  RecoveryBegun = 17,
  IncidentClosed = 18,
  AuthorityRolledOver = 19,
  PolicyAdopted = 20,
  TargetBindingRegistered = 21,
  StoreRecovered = 22,
};

inline constexpr std::uint8_t kJournalKindCount = 22;

[[nodiscard]] std::string_view journal_kind_name(JournalKind kind) noexcept;
[[nodiscard]] std::optional<JournalKind> journal_kind_from_value(std::uint8_t value) noexcept;

struct IncidentOpenedPayload {
  ThermalZoneRef trigger_zone{};
  Severity initial{Severity::Nominal};
};

struct ZoneRegisteredPayload {
  ThermalZoneRef zone{};
};

struct ProbeRegisteredPayload {
  ThermalZoneRef zone{};
  ThermalProbeRef probe{};
};

struct ObligationRegisteredPayload {
  ProtectedObligation obligation{};
};

struct EvidenceAdmittedPayload {
  ThermalSample sample{};
};

struct ObligationStatusPayload {
  ObligationId obligation{};
  ObligationStatus status{ObligationStatus::Unknown};
  Timestamp status_at{};
  ExternalAuthorityRef source{};
};

struct TickPayload {
  std::uint32_t evaluated_zones{0};
};

struct RequestsIssuedPayload {
  std::vector<MitigationRequest> requests{};
};

struct RequestAcknowledgedPayload {
  MitigationRequestId request{};
  ExternalEvidenceRef external_ref{};
};

struct RequestObservedPayload {
  MitigationRequestId request{};
  ExternalEvidenceRef external_ref{};
  ObservationSequence observation{};
};

struct RequestVerifiedPayload {
  MitigationRequestId request{};
  ExternalEvidenceRef verification_ref{};
};

struct RequestFailedPayload {
  MitigationRequestId request{};
  FailureCode failure{FailureCode::None};
  std::int32_t transport_code{0};
};

struct RequestSupersededPayload {
  MitigationRequestId request{};
  SupersedeReason reason{SupersedeReason::None};
};

struct RequestAbandonedPayload {
  MitigationRequestId request{};
};

struct ConstraintRelaxedPayload {
  ObligationId obligation{};
  MitigationClass cls{MitigationClass::DerateAccelerators};
  ExternalAuthorityRef authority{};
};

struct ConstraintReimposedPayload {
  ObligationId obligation{};
};

struct RecoveryBegunPayload {
  OperatorRef operator_ref{};
};

struct IncidentClosedPayload {
  OperatorRef operator_ref{};
  Disposition disposition{Disposition::Recovered};
};

// A new controller incarnation takes authority. Every non-terminal request from
// the previous authority is fenced, and every recovery timer is reset: a new
// authority must re-earn recovery from current evidence.
struct AuthorityRolledOverPayload {
  ControllerIncarnation incarnation{};
  ControlEpoch epoch{};
};

struct PolicyAdoptedPayload {
  ThermalPolicy policy{};
  RelaxationPolicy relaxation{};
};

struct TargetBindingRegisteredPayload {
  ThermalZoneRef zone{};
  PowerDomainRef power_domain{};
  std::vector<EquipmentRef> equipment{};
};

// Written once when a runtime opens a store that already contained durable
// state. It is what turns persisted dynamic observations into explicitly
// non-current evidence: no restored reading counts towards a gate, and any
// dispatch that was in flight when the previous incarnation stopped is
// resolved as indeterminate rather than retried under the same attempt id.
struct StoreRecoveredPayload {
  bool rollover{false};
  ControllerIncarnation previous_incarnation{};
  ControlEpoch previous_epoch{};
};

using JournalPayload =
    std::variant<IncidentOpenedPayload, ZoneRegisteredPayload, ProbeRegisteredPayload,
                 ObligationRegisteredPayload, EvidenceAdmittedPayload, ObligationStatusPayload,
                 TickPayload, RequestsIssuedPayload, RequestAcknowledgedPayload,
                 RequestObservedPayload, RequestVerifiedPayload, RequestFailedPayload,
                 RequestSupersededPayload, RequestAbandonedPayload, ConstraintRelaxedPayload,
                 ConstraintReimposedPayload, RecoveryBegunPayload, IncidentClosedPayload,
                 AuthorityRolledOverPayload, PolicyAdoptedPayload, TargetBindingRegisteredPayload,
                 StoreRecoveredPayload>;

// One authoritative input, bound to the authority that accepted it and to the
// state revision it produced.
struct JournalEntry {
  JournalSequence sequence{};
  Timestamp at{};
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision revision_before{};
  StateRevision revision_after{};
  IncidentId incident{};
  JournalKind kind{JournalKind::Tick};
  JournalPayload payload{};
};

// Why a severity or lifecycle transition happened.
enum class TransitionTrigger : std::uint8_t {
  IncidentOpened = 1,
  EvidenceEscalation = 2,
  RateEscalation = 3,
  EvidenceLoss = 4,
  MitigationFailure = 5,
  RecoveryStep = 6,
  RecoveryBegun = 7,
  RecoveryCompleted = 8,
  AuthorityRollover = 9,
  IncidentClosed = 10,
  AdoptedState = 11,
};

[[nodiscard]] std::string_view transition_trigger_name(TransitionTrigger trigger) noexcept;
[[nodiscard]] std::optional<TransitionTrigger> transition_trigger_from_value(std::uint8_t value) noexcept;

// A recorded severity or lifecycle change. Produced by apply and retained for
// audit output.
struct TransitionRecord {
  TransitionSequence sequence{};
  Timestamp at{};
  StateRevision revision{};
  IncidentId incident{};
  Severity from_severity{Severity::Nominal};
  Severity to_severity{Severity::Nominal};
  Lifecycle from_lifecycle{Lifecycle::None};
  Lifecycle to_lifecycle{Lifecycle::None};
  TransitionTrigger trigger{TransitionTrigger::IncidentOpened};
  std::string detail{};
};

}  // namespace summon::tem
