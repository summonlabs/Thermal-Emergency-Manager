// Thermal Emergency Manager -- authoritative domain state and replay projection.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tem/evidence.hpp"
#include "tem/journal.hpp"
#include "tem/mitigation.hpp"
#include "tem/obligations.hpp"
#include "tem/recovery.hpp"
#include "tem/severity.hpp"

namespace summon::tem {

// Facility configuration and current evidence for one probe. live_since_recovery
// is false for every slot restored from durable state: a restored reading is
// not current physical evidence, and stays ineligible until a new live sample
// for that probe is admitted.
struct ProbeSlot {
  ThermalProbeRef probe{};
  bool present{false};
  bool live_since_recovery{false};
  ThermalSample sample{};
  JournalSequence recorded_at{};
  ObservationSequence last_sequence{};
};

struct ZoneSlot {
  ThermalZoneRef zone{};
  std::vector<ProbeSlot> probes{};
};

struct ObligationSlot {
  ProtectedObligation obligation{};
};

// Opaque facility binding supplied by the authority that owns facility
// topology. This runtime never discovers or decides topology: it stores opaque
// references so that a thermal zone can be addressed by the power domain and
// equipment that serve it.
struct TargetBinding {
  ThermalZoneRef zone{};
  PowerDomainRef power_domain{};
  std::vector<EquipmentRef> equipment{};
};

// Everything derived about one emergency. Severity and lifecycle are latched
// here; they move only through the documented rules applied by apply().
struct IncidentProjection {
  IncidentId id{};
  IncidentGeneration generation{};
  ThermalZoneRef trigger_zone{};

  Severity severity{Severity::Nominal};
  Severity peak_severity{Severity::Nominal};
  Lifecycle lifecycle{Lifecycle::None};

  Timestamp opened_at{};
  Timestamp closed_at{};
  bool closed{false};

  // Margin tracking used by the dwell gate. margin_ok means every monitored
  // zone was inside the hysteresis band at the last evaluation.
  bool margin_ok{false};
  Timestamp margin_since{};
  bool recovered_ok{false};
  Timestamp recovered_since{};

  Timestamp stabilizing_since{};
  Timestamp last_escalation_at{};
  bool has_last_escalation{false};
  Timestamp last_deescalation_at{};
  bool has_last_deescalation{false};
  Timestamp began_recovery_at{};
  bool has_begun_recovery{false};

  std::vector<MitigationRecord> requests{};
  std::vector<RelaxationRecord> relaxations{};
  std::vector<TransitionRecord> transitions{};
  std::uint64_t transitions_dropped{0};
  std::uint64_t fenced_requests{0};

  Disposition disposition{Disposition::Recovered};
  bool has_disposition{false};
  OperatorRef closed_by{};

  [[nodiscard]] bool is_open_incident() const noexcept {
    return !closed && lifecycle != Lifecycle::None;
  }
};

// The complete authoritative state. It is the persisted unit and the replay
// projection: replaying the journal over a checkpoint must reproduce this
// structure byte for byte.
struct DomainState {
  ControllerIncarnation incarnation{};
  ControlEpoch epoch{};
  StateRevision revision{};
  JournalSequence journal_sequence{};
  TransitionSequence transition_sequence{};

  IncidentId next_incident_id{IncidentId::from_value(1)};
  IncidentGeneration next_generation{IncidentGeneration::from_value(1)};
  MitigationRequestId next_request_id{MitigationRequestId::from_value(1)};
  CommandId next_command_id{CommandId::from_value(1)};
  AttemptId next_attempt_id{AttemptId::from_value(1)};
  ObligationId next_obligation_id{ObligationId::from_value(1)};

  IncidentId current_incident{};
  IncidentGeneration current_generation{};

  std::vector<ZoneSlot> zones{};
  std::vector<ObligationSlot> obligations{};
  std::vector<TargetBinding> bindings{};
  std::vector<IncidentProjection> incidents{};

  ThermalPolicy policy{};
  RelaxationPolicy relaxation{};
  bool recovered_from_store{false};

  // Instant of the most recent applied input. Used to refuse a clock that moves
  // backwards, which would otherwise corrupt every dwell and staleness
  // computation silently.
  Timestamp last_input_at{};
};

// ---------------------------------------------------------------------------
// Lookup helpers. All return nullptr when absent; none of them create state.
// ---------------------------------------------------------------------------

[[nodiscard]] IncidentProjection* find_incident(DomainState& state, IncidentId id) noexcept;
[[nodiscard]] const IncidentProjection* find_incident(const DomainState& state, IncidentId id) noexcept;
[[nodiscard]] ZoneSlot* find_zone(DomainState& state, const ThermalZoneRef& zone) noexcept;
[[nodiscard]] const ZoneSlot* find_zone(const DomainState& state, const ThermalZoneRef& zone) noexcept;
[[nodiscard]] ProbeSlot* find_probe(ZoneSlot& zone, const ThermalProbeRef& probe) noexcept;
[[nodiscard]] const ProbeSlot* find_probe(const ZoneSlot& zone, const ThermalProbeRef& probe) noexcept;
[[nodiscard]] MitigationRecord* find_request(IncidentProjection& incident, MitigationRequestId id) noexcept;
[[nodiscard]] const MitigationRecord* find_request(const IncidentProjection& incident,
                                                  MitigationRequestId id) noexcept;
[[nodiscard]] MitigationRecord* find_request_by_key(IncidentProjection& incident,
                                                    IdempotencyKey key) noexcept;
[[nodiscard]] const MitigationRecord* find_request_by_key(const IncidentProjection& incident,
                                                          IdempotencyKey key) noexcept;
[[nodiscard]] ObligationSlot* find_obligation(DomainState& state, ObligationId id) noexcept;
[[nodiscard]] const ObligationSlot* find_obligation(const DomainState& state,
                                                    ObligationId id) noexcept;
[[nodiscard]] TargetBinding* find_binding(DomainState& state, const ThermalZoneRef& zone) noexcept;
[[nodiscard]] const TargetBinding* find_binding(const DomainState& state,
                                                const ThermalZoneRef& zone) noexcept;

// Validates that a decoded or constructed state respects its own configured
// bounds. Decoding checks hard limits; this checks the configured ones.
[[nodiscard]] Status validate_state_limits(const DomainState& state);

// Requests in deterministic dispatch order: class ordinal, then target bytes,
// then request id.
[[nodiscard]] std::vector<const MitigationRecord*> ordered_requests(const IncidentProjection& incident);

// The obligation that blocks a request, if any: a covering obligation that
// forbids the class and has not been relaxed.
[[nodiscard]] const ProtectedObligation* find_blocking_obligation(const DomainState& state,
                                                                 const IncidentProjection& incident,
                                                                 const RequestTarget& target,
                                                                 MitigationClass cls);

// ---------------------------------------------------------------------------
// Derivation
// ---------------------------------------------------------------------------

// Assessment of every monitored zone at an instant.
[[nodiscard]] EvidenceAssessment assess_state(const DomainState& state, Timestamp now);

// Recovery evaluation. When for_closure is false the ClosureDwell gate is
// reported as NotApplicable. The result is a pure function of the state and
// the instant; it is recomputed rather than cached so that stale conclusions
// can never be reused as authority.
[[nodiscard]] RecoveryEligibility evaluate_recovery(const DomainState& state, Timestamp now,
                                                    bool for_closure);

// True when every gate other than Dwell and ClosureDwell passes. This is the
// precondition for entering Stabilizing and for remaining in Recovering.
[[nodiscard]] bool recovery_preconditions_hold(const RecoveryEligibility& eligibility);

// True when the hysteresis margin for the incident's current severity holds
// across every monitored zone.
[[nodiscard]] bool margin_condition(const DomainState& state, const IncidentProjection& incident,
                                    const EvidenceAssessment& assessment);

// ---------------------------------------------------------------------------
// The single mutator: applying an authoritative input to the state.
//
// apply() is pure with respect to everything except the state it is given: it
// reads no clock, no environment, and no external service. Replay therefore
// reproduces the same state from the same inputs, and a mismatch is reported
// as ReplayDivergence rather than silently accepted.
//
// Preconditions checked by apply (a mismatch is a divergence, not a user
// error): the entry revision_before must equal the state revision, the entry
// sequence must be exactly one past the state sequence.
// ---------------------------------------------------------------------------

struct ApplyOutcome {
  bool state_changed{false};
  std::uint32_t requests_issued{0};
  std::uint32_t requests_failed{0};
  std::uint32_t requests_expired{0};
  std::uint32_t requests_abandoned{0};
  Severity severity_before{Severity::Nominal};
  Severity severity_after{Severity::Nominal};
  Lifecycle lifecycle_before{Lifecycle::None};
  Lifecycle lifecycle_after{Lifecycle::None};
};

[[nodiscard]] Result<ApplyOutcome> apply(DomainState& state, const JournalEntry& entry);

// Replays a journal over a checkpoint and returns the reconstructed state.
[[nodiscard]] Result<DomainState> replay(const DomainState& checkpoint,
                                         const std::vector<JournalEntry>& journal);

// Byte-canonical equality over the persisted representation. Used by replay
// verification: two states are equal only when they encode identically.
[[nodiscard]] bool states_encode_identically(const DomainState& a, const DomainState& b);

}  // namespace summon::tem
