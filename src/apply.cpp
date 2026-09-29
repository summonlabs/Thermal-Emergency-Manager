// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <algorithm>
#include <string>
#include <vector>

#include "tem/plan.hpp"
#include "tem/state.hpp"

namespace summon::tem {
namespace {

void trim_detail(std::string& text, const Bounds& bounds) {
  if (text.size() > bounds.max_description_length) {
    text.resize(bounds.max_description_length);
  }
}

void record_transition(DomainState& state, IncidentProjection& incident, const JournalEntry& entry,
                       Severity from_severity, Severity to_severity, Lifecycle from_lifecycle,
                       Lifecycle to_lifecycle, TransitionTrigger trigger, std::string detail) {
  TransitionRecord record;
  record.sequence = state.transition_sequence;
  state.transition_sequence = state.transition_sequence.next();
  record.at = entry.at;
  record.revision = entry.revision_after;
  record.incident = incident.id;
  record.from_severity = from_severity;
  record.to_severity = to_severity;
  record.from_lifecycle = from_lifecycle;
  record.to_lifecycle = to_lifecycle;
  record.trigger = trigger;
  trim_detail(detail, state.policy.bounds);
  record.detail = std::move(detail);

  incident.transitions.push_back(std::move(record));
  const std::size_t limit = state.policy.bounds.max_transitions_per_incident;
  while (incident.transitions.size() > limit) {
    incident.transitions.erase(incident.transitions.begin());
    ++incident.transitions_dropped;
  }
}

bool same_transition(Severity from_severity, Severity to_severity, Lifecycle from_lifecycle,
                     Lifecycle to_lifecycle) noexcept {
  return from_severity == to_severity && from_lifecycle == to_lifecycle;
}

void set_request_state(MitigationRecord& record, MitigationState next, Timestamp at,
                       FailureCode failure, SupersedeReason supersede, std::int32_t code,
                       const Bounds& bounds) {
  record.state = next;
  record.state_at = at;
  record.failure = failure;
  record.supersede = supersede;
  record.transport_code = code;
  RequestHistoryPoint point;
  point.state = next;
  point.at = at;
  point.failure = failure;
  point.supersede = supersede;
  point.detail = code;
  record.history.push_back(point);
  while (record.history.size() > bounds.max_request_history) {
    record.history.erase(record.history.begin());
  }
}

Result<MitigationRecord*> require_request(IncidentProjection& incident, MitigationRequestId id) {
  MitigationRecord* record = find_request(incident, id);
  if (record == nullptr) {
    return Status::error(StatusCode::RequestNotFound, "mitigation request is not recorded")
        .with_context(std::to_string(id.value()));
  }
  return record;
}

// Which severity transition trigger explains an escalation.
TransitionTrigger escalation_trigger(const EvidenceAssessment& assessment,
                                     const ThermalPolicy& policy) {
  for (const ZoneAssessment& zone : assessment.zones) {
    if (zone.cls != FreshnessClass::Fresh || !zone.has_rate) {
      continue;
    }
    if (zone.peak_rate >= policy.rapid_rate) {
      return TransitionTrigger::RateEscalation;
    }
  }
  return TransitionTrigger::EvidenceEscalation;
}

Result<ApplyOutcome> apply_incident_opened(DomainState& state, const JournalEntry& entry) {
  const auto& payload = std::get<IncidentOpenedPayload>(entry.payload);
  const IncidentProjection* current = find_incident(state, state.current_incident);
  if (current != nullptr && !current->closed) {
    return Status::error(StatusCode::IncidentAlreadyOpen,
                         "an incident is already open for this facility");
  }
  if (find_zone(state, payload.trigger_zone) == nullptr) {
    return Status::error(StatusCode::EvidenceUnknownTarget, "trigger zone is not registered")
        .with_context(payload.trigger_zone.text());
  }
  if (state.incidents.size() >= state.policy.bounds.max_incidents) {
    // Retire the oldest closed incident; a live incident is never evicted.
    auto evictable = std::find_if(state.incidents.begin(), state.incidents.end(),
                                  [](const IncidentProjection& incident) { return incident.closed; });
    if (evictable == state.incidents.end()) {
      return Status::error(StatusCode::ResourceExhausted,
                           "incident table is full and no closed incident can be retired");
    }
    state.incidents.erase(evictable);
  }

  const EvidenceAssessment assessment = assess_state(state, entry.at);
  const Severity justified = justified_severity(assessment, state.policy);
  if (justified != payload.initial) {
    return Status::error(StatusCode::ReplayDivergence,
                         "recorded opening severity does not match the evidence at that instant");
  }
  if (severity_ordinal(justified) < severity_ordinal(Severity::Advisory)) {
    return Status::error(StatusCode::PreconditionFailed,
                         "an incident cannot be opened without at least advisory evidence");
  }

  IncidentProjection incident;
  incident.id = state.next_incident_id;
  incident.generation = state.next_generation;
  incident.trigger_zone = payload.trigger_zone;
  incident.severity = justified;
  incident.peak_severity = justified;
  incident.lifecycle = Lifecycle::Active;
  incident.opened_at = entry.at;
  incident.last_escalation_at = entry.at;
  incident.has_last_escalation = true;
  incident.margin_ok = false;
  incident.margin_since = entry.at;

  state.next_incident_id = state.next_incident_id.next();
  state.next_generation = state.next_generation.next();
  state.current_incident = incident.id;
  state.current_generation = incident.generation;

  record_transition(state, incident, entry, Severity::Nominal, justified, Lifecycle::None,
                    Lifecycle::Active, TransitionTrigger::IncidentOpened,
                    std::string("opened on ") + payload.trigger_zone.text());
  incident.margin_ok = margin_condition(state, incident, assessment);
  incident.margin_since = entry.at;

  state.incidents.push_back(std::move(incident));

  ApplyOutcome outcome;
  outcome.state_changed = true;
  outcome.severity_before = Severity::Nominal;
  outcome.severity_after = justified;
  outcome.lifecycle_before = Lifecycle::None;
  outcome.lifecycle_after = Lifecycle::Active;
  return outcome;
}

Result<ApplyOutcome> apply_tick(DomainState& state, const JournalEntry& entry) {
  ApplyOutcome outcome;
  IncidentProjection* incident = find_incident(state, state.current_incident);
  if (incident == nullptr || incident->closed) {
    return outcome;  // no live incident: a tick is a no-op and is not journalled
  }

  const Bounds& bounds = state.policy.bounds;
  outcome.severity_before = incident->severity;
  outcome.lifecycle_before = incident->lifecycle;

  const EvidenceAssessment assessment = assess_state(state, entry.at);
  const Severity justified = justified_severity(assessment, state.policy);
  const bool evidence_lost = !assessment.all_fresh && !state.zones.empty();
  bool changed = false;

  // (1) Request validity windows. Deterministic order: the request table is
  // ordered by ascending request id, which is its insertion order.
  for (MitigationRecord& record : incident->requests) {
    if (record.is_terminal()) {
      continue;
    }
    if (record.request.has_expiry && entry.at > record.request.expires_at &&
        (record.state == MitigationState::Planned || record.state == MitigationState::Issued)) {
      set_request_state(record, MitigationState::Expired, entry.at, FailureCode::ExpiryElapsed,
                        SupersedeReason::None, 0, bounds);
      ++outcome.requests_expired;
      changed = true;
      continue;
    }
    if (record.request.has_verification_deadline &&
        entry.at > record.request.verification_deadline &&
        (record.state == MitigationState::Acknowledged ||
         record.state == MitigationState::Observed ||
         record.state == MitigationState::Issued)) {
      set_request_state(record, MitigationState::Failed, entry.at,
                        FailureCode::VerificationTimeout, SupersedeReason::None, 0, bounds);
      ++outcome.requests_failed;
      changed = true;
    }
  }

  // (2) Escalation. Severity is monotonic while the incident is not in the
  // recovery path; it never decreases here.
  Severity previous_severity = incident->severity;
  Lifecycle previous_lifecycle = incident->lifecycle;

  if (severity_ordinal(justified) > severity_ordinal(incident->severity)) {
    incident->severity = justified;
    if (severity_ordinal(justified) > severity_ordinal(incident->peak_severity)) {
      incident->peak_severity = justified;
    }
    incident->last_escalation_at = entry.at;
    incident->has_last_escalation = true;
    record_transition(state, *incident, entry, previous_severity, incident->severity,
                      previous_lifecycle, incident->lifecycle,
                      escalation_trigger(assessment, state.policy),
                      std::string("evidence justifies ") + std::string(severity_name(justified)));
    previous_severity = incident->severity;
    changed = true;
  } else if (state.policy.escalate_on_evidence_loss && evidence_lost &&
             severity_ordinal(incident->severity) < severity_ordinal(Severity::Warning)) {
    incident->severity = Severity::Warning;
    previous_severity = Severity::Warning;
    if (severity_ordinal(Severity::Warning) > severity_ordinal(incident->peak_severity)) {
      incident->peak_severity = Severity::Warning;
    }
    record_transition(state, *incident, entry, Severity::Nominal, Severity::Warning,
                      previous_lifecycle, incident->lifecycle, TransitionTrigger::EvidenceLoss,
                      "current evidence was lost during an open incident");
    changed = true;
  }

  // (3) Hysteresis anchor.
  const bool margin_now = margin_condition(state, *incident, assessment);
  if (margin_now != incident->margin_ok) {
    incident->margin_ok = margin_now;
    incident->margin_since = entry.at;
    changed = true;
  }

  // (4) Lifecycle movement.
  const RecoveryEligibility eligibility = evaluate_recovery(state, entry.at, false);
  const bool preconditions = recovery_preconditions_hold(eligibility);
  switch (incident->lifecycle) {
    case Lifecycle::Active:
      if (preconditions && incident->margin_ok) {
        incident->lifecycle = Lifecycle::Stabilizing;
        incident->stabilizing_since = entry.at;
        record_transition(state, *incident, entry, incident->severity, incident->severity,
                          Lifecycle::Active, Lifecycle::Stabilizing,
                          TransitionTrigger::RecoveryStep,
                          "recovery preconditions hold; awaiting an explicit recovery request");
        changed = true;
      }
      break;
    case Lifecycle::Stabilizing:
      if (!preconditions) {
        incident->lifecycle = Lifecycle::Active;
        record_transition(state, *incident, entry, incident->severity, incident->severity,
                          Lifecycle::Stabilizing, Lifecycle::Active,
                          TransitionTrigger::RecoveryStep,
                          "recovery preconditions no longer hold");
        changed = true;
      }
      break;
    case Lifecycle::Recovering: {
      if (!preconditions) {
        incident->lifecycle = Lifecycle::Active;
        incident->recovered_ok = false;
        record_transition(state, *incident, entry, incident->severity, incident->severity,
                          Lifecycle::Recovering, Lifecycle::Active, TransitionTrigger::RecoveryStep,
                          "recovery preconditions no longer hold");
        changed = true;
        break;
      }
      const bool dwell_ok = eligibility.result_of(RecoveryGate::Dwell) == GateResult::Passed;
      bool spacing_ok = true;
      if (incident->has_last_deescalation) {
        const std::int64_t since =
            entry.at.unix_nanos() - incident->last_deescalation_at.unix_nanos();
        spacing_ok = since >= state.policy.deescalation_dwell.nanos();
      }
      if (dwell_ok && spacing_ok && incident->severity != Severity::Nominal) {
        const Severity candidate = severity_max(
            severity_from_ordinal(severity_ordinal(incident->severity) - 1u), justified);
        if (severity_ordinal(candidate) < severity_ordinal(incident->severity)) {
          const Severity from = incident->severity;
          incident->severity = candidate;
          incident->last_deescalation_at = entry.at;
          incident->has_last_deescalation = true;
          if (candidate == Severity::Nominal) {
            incident->lifecycle = Lifecycle::Recovered;
            incident->recovered_ok = true;
            incident->recovered_since = entry.at;
            record_transition(state, *incident, entry, from, candidate, Lifecycle::Recovering,
                              Lifecycle::Recovered, TransitionTrigger::RecoveryCompleted,
                              "recovery conditions verified at nominal severity");
          } else {
            record_transition(state, *incident, entry, from, candidate, Lifecycle::Recovering,
                              Lifecycle::Recovering, TransitionTrigger::RecoveryStep,
                              std::string("de-escalated to ") + std::string(severity_name(candidate)));
          }
          changed = true;
        }
      }
      break;
    }
    case Lifecycle::Recovered:
      if (!preconditions || justified != Severity::Nominal) {
        incident->lifecycle = Lifecycle::Active;
        incident->recovered_ok = false;
        record_transition(state, *incident, entry, incident->severity, incident->severity,
                          Lifecycle::Recovered, Lifecycle::Active,
                          TransitionTrigger::EvidenceEscalation,
                          "recovered state no longer holds under current evidence");
        changed = true;
      }
      break;
    case Lifecycle::None:
    case Lifecycle::Closed:
      break;
  }

  // (5) Requests that are no longer required are abandoned, never left open.
  for (MitigationRecord& record : incident->requests) {
    if (record.is_terminal()) {
      continue;
    }
    if (!request_still_required(state, *incident, assessment, record)) {
      set_request_state(record, MitigationState::Abandoned, entry.at, FailureCode::None,
                        SupersedeReason::SeverityReduced, 0, bounds);
      ++outcome.requests_abandoned;
      changed = true;
    }
  }

  // (6) Defensive fencing: nothing non-terminal may outlive its epoch.
  for (MitigationRecord& record : incident->requests) {
    if (record.is_terminal() || record.request.epoch == state.epoch) {
      continue;
    }
    set_request_state(record, MitigationState::Superseded, entry.at, FailureCode::None,
                      SupersedeReason::AuthorityRollover, 0, bounds);
    ++incident->fenced_requests;
    changed = true;
  }

  outcome.severity_after = incident->severity;
  outcome.lifecycle_after = incident->lifecycle;
  outcome.state_changed = changed;
  return outcome;
}

}  // namespace

Result<ApplyOutcome> apply(DomainState& state, const JournalEntry& entry) {
  if (entry.revision_before != state.revision) {
    return Status::error(StatusCode::ReplayDivergence,
                         "journal entry revision does not match the state revision");
  }
  const std::uint64_t expected_sequence = state.journal_sequence.value() + 1u;
  if (entry.sequence.value() != expected_sequence) {
    return Status::error(StatusCode::ReplayDivergence,
                         "journal entry sequence is not contiguous with the state");
  }
  if (static_cast<std::uint8_t>(entry.kind) != entry.payload.index() + 1) {
    return Status::error(StatusCode::MalformedEncoding,
                         "journal kind does not match its payload alternative");
  }

  const Bounds& bounds = state.policy.bounds;
  ApplyOutcome outcome;

  if (entry.kind == JournalKind::Tick) {
    auto tick = apply_tick(state, entry);
    if (!tick.ok()) {
      return tick.status();
    }
    outcome = std::move(tick).value();
    if (!outcome.state_changed) {
      // A tick that changes nothing is discarded by the caller and is never
      // journalled, so its revision claim is irrelevant and no state is
      // touched.
      return outcome;
    }
    if (entry.revision_after.value() != entry.revision_before.value() + 1u) {
      return Status::error(StatusCode::ReplayDivergence,
                           "a state change must advance the revision by exactly one");
    }
    state.revision = entry.revision_after;
    state.journal_sequence = entry.sequence;
    state.last_input_at = entry.at;
    return outcome;
  }

  if (entry.revision_after.value() != entry.revision_before.value() + 1u) {
    return Status::error(StatusCode::ReplayDivergence,
                         "a journalled input must advance the revision by exactly one");
  }
  state.revision = entry.revision_after;
  state.journal_sequence = entry.sequence;
  state.last_input_at = entry.at;
  outcome.state_changed = true;

  IncidentProjection* incident = find_incident(state, state.current_incident);
  switch (entry.kind) {
    case JournalKind::IncidentOpened:
      return apply_incident_opened(state, entry);

    case JournalKind::ZoneRegistered: {
      const auto& payload = std::get<ZoneRegisteredPayload>(entry.payload);
      if (find_zone(state, payload.zone) != nullptr) {
        return Status::error(StatusCode::DuplicateElement, "thermal zone is already registered")
            .with_context(payload.zone.text());
      }
      if (state.zones.size() >= bounds.max_zones) {
        return Status::error(StatusCode::ResourceExhausted, "zone table is full");
      }
      ZoneSlot slot;
      slot.zone = payload.zone;
      state.zones.push_back(std::move(slot));
      break;
    }

    case JournalKind::ProbeRegistered: {
      const auto& payload = std::get<ProbeRegisteredPayload>(entry.payload);
      ZoneSlot* zone = find_zone(state, payload.zone);
      if (zone == nullptr) {
        return Status::error(StatusCode::EvidenceUnknownTarget, "thermal zone is not registered")
            .with_context(payload.zone.text());
      }
      if (find_probe(*zone, payload.probe) != nullptr) {
        return Status::error(StatusCode::DuplicateElement, "probe is already registered")
            .with_context(payload.probe.text());
      }
      if (zone->probes.size() >= bounds.max_probes_per_zone) {
        return Status::error(StatusCode::ResourceExhausted, "probe table for the zone is full")
            .with_context(payload.zone.text());
      }
      ProbeSlot slot;
      slot.probe = payload.probe;
      zone->probes.push_back(std::move(slot));
      break;
    }

    case JournalKind::TargetBindingRegistered: {
      const auto& payload = std::get<TargetBindingRegisteredPayload>(entry.payload);
      if (find_zone(state, payload.zone) == nullptr) {
        return Status::error(StatusCode::EvidenceUnknownTarget, "thermal zone is not registered")
            .with_context(payload.zone.text());
      }
      if (find_binding(state, payload.zone) != nullptr) {
        return Status::error(StatusCode::DuplicateElement,
                             "a target binding already exists for the zone")
            .with_context(payload.zone.text());
      }
      if (state.bindings.size() >= bounds.max_zones) {
        return Status::error(StatusCode::ResourceExhausted, "target binding table is full");
      }
      if (payload.equipment.size() > bounds.max_probes_per_zone) {
        return Status::error(StatusCode::BoundsExceeded,
                             "target binding exceeds the equipment bound");
      }
      TargetBinding binding;
      binding.zone = payload.zone;
      binding.power_domain = payload.power_domain;
      binding.equipment = payload.equipment;
      state.bindings.push_back(std::move(binding));
      break;
    }

    case JournalKind::ObligationRegistered: {
      const auto& payload = std::get<ObligationRegisteredPayload>(entry.payload);
      if (!(payload.obligation.id == state.next_obligation_id)) {
        return Status::error(StatusCode::ReplayDivergence,
                             "obligation id does not match the allocation sequence");
      }
      if (state.obligations.size() >= bounds.max_obligations) {
        return Status::error(StatusCode::ResourceExhausted, "obligation table is full");
      }
      ObligationSlot slot;
      slot.obligation = payload.obligation;
      state.obligations.push_back(std::move(slot));
      state.next_obligation_id = state.next_obligation_id.next();
      break;
    }

    case JournalKind::EvidenceAdmitted: {
      const auto& payload = std::get<EvidenceAdmittedPayload>(entry.payload);
      ZoneSlot* zone = find_zone(state, payload.sample.zone);
      if (zone == nullptr) {
        return Status::error(StatusCode::EvidenceUnknownTarget, "thermal zone is not registered")
            .with_context(payload.sample.zone.text());
      }
      ProbeSlot* probe = find_probe(*zone, payload.sample.probe);
      if (probe == nullptr) {
        return Status::error(StatusCode::EvidenceUnknownTarget, "probe is not registered")
            .with_context(payload.sample.probe.text());
      }
      if (probe->present && !(probe->last_sequence < payload.sample.sequence)) {
        return Status::error(StatusCode::EvidenceOutOfOrder,
                             "sample sequence does not advance the probe stream")
            .with_context(payload.sample.probe.text());
      }
      probe->sample = payload.sample;
      probe->present = true;
      probe->recorded_at = entry.sequence;
      probe->last_sequence = payload.sample.sequence;
      probe->live_since_recovery = payload.sample.origin == SampleOrigin::Live;
      break;
    }

    case JournalKind::ObligationStatusReported: {
      const auto& payload = std::get<ObligationStatusPayload>(entry.payload);
      ObligationSlot* slot = find_obligation(state, payload.obligation);
      if (slot == nullptr) {
        return Status::error(StatusCode::ObligationUnknown, "obligation is not registered")
            .with_context(std::to_string(payload.obligation.value()));
      }
      if (slot->obligation.status_present && payload.status_at < slot->obligation.status_at) {
        return Status::error(StatusCode::EvidenceOutOfOrder,
                             "obligation status report is older than the recorded one");
      }
      slot->obligation.status = payload.status;
      slot->obligation.status_at = payload.status_at;
      slot->obligation.status_source = payload.source;
      slot->obligation.status_present = true;
      break;
    }

    case JournalKind::RequestsIssued: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident,
                             "mitigation requests require an open incident");
      }
      const auto& payload = std::get<RequestsIssuedPayload>(entry.payload);
      if (payload.requests.size() > bounds.max_requests_per_issue) {
        return Status::error(StatusCode::BoundsExceeded, "too many requests in one issuance");
      }
      if (incident->requests.size() + payload.requests.size() > bounds.max_requests_per_incident) {
        return Status::error(StatusCode::ResourceExhausted,
                             "mitigation request table for the incident is full");
      }
      MitigationRequestId expected_id = state.next_request_id;
      AttemptId expected_attempt = state.next_attempt_id;
      CommandId expected_command = state.next_command_id;
      for (const MitigationRequest& request : payload.requests) {
        if (!(request.id == expected_id) || !(request.attempt == expected_attempt) ||
            !(request.command == expected_command)) {
          return Status::error(StatusCode::ReplayDivergence,
                               "issued request identity does not match the allocation sequence");
        }
        if (request.planned_revision != entry.revision_after) {
          return Status::error(StatusCode::ReplayDivergence,
                               "issued request is not planned against this revision");
        }
        if (find_request(*incident, request.id) != nullptr) {
          return Status::error(StatusCode::DuplicateElement,
                               "mitigation request id is already recorded");
        }
        MitigationRecord record;
        record.request = request;
        record.state = MitigationState::Planned;
        record.state_at = entry.at;
        RequestHistoryPoint point;
        point.state = MitigationState::Planned;
        point.at = entry.at;
        record.history.push_back(point);
        incident->requests.push_back(std::move(record));
        expected_id = expected_id.next();
        expected_attempt = expected_attempt.next();
        expected_command = expected_command.next();
      }
      state.next_request_id = expected_id;
      state.next_attempt_id = expected_attempt;
      state.next_command_id = expected_command;
      outcome.requests_issued = static_cast<std::uint32_t>(payload.requests.size());
      break;
    }

    case JournalKind::RequestAcknowledged: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<RequestAcknowledgedPayload>(entry.payload);
      auto record = require_request(*incident, payload.request);
      if (!record.ok()) {
        return record.status();
      }
      MitigationRecord& target = *record.value();
      if (target.state != MitigationState::Planned && target.state != MitigationState::Issued) {
        return Status::error(StatusCode::RequestStateConflict,
                             "only a planned or issued request can be acknowledged")
            .with_context(std::string(mitigation_state_name(target.state)));
      }
      set_request_state(target, MitigationState::Acknowledged, entry.at, FailureCode::None,
                        SupersedeReason::None, target.transport_code, bounds);
      target.external_ref = payload.external_ref;
      target.has_external_ref = payload.external_ref.valid();
      break;
    }

    case JournalKind::RequestObserved: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<RequestObservedPayload>(entry.payload);
      auto record = require_request(*incident, payload.request);
      if (!record.ok()) {
        return record.status();
      }
      MitigationRecord& target = *record.value();
      if (target.state != MitigationState::Issued && target.state != MitigationState::Acknowledged) {
        return Status::error(StatusCode::RequestStateConflict,
                             "only an issued or acknowledged request can be observed")
            .with_context(std::string(mitigation_state_name(target.state)));
      }
      set_request_state(target, MitigationState::Observed, entry.at, FailureCode::None,
                        SupersedeReason::None, target.transport_code, bounds);
      target.external_ref = payload.external_ref;
      target.has_external_ref = payload.external_ref.valid();
      target.last_observation = payload.observation;
      break;
    }

    case JournalKind::RequestVerified: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<RequestVerifiedPayload>(entry.payload);
      auto record = require_request(*incident, payload.request);
      if (!record.ok()) {
        return record.status();
      }
      MitigationRecord& target = *record.value();
      if (target.state == MitigationState::Verified) {
        // Verification evidence may be refreshed; the terminal state is not
        // changed, but the currency of the evidence is.
        target.verified_at = entry.at;
        target.has_verified_at = true;
        target.verification_ref = payload.verification_ref;
        target.has_verification_ref = payload.verification_ref.valid();
        RequestHistoryPoint point;
        point.state = MitigationState::Verified;
        point.at = entry.at;
        target.history.push_back(point);
        while (target.history.size() > bounds.max_request_history) {
          target.history.erase(target.history.begin());
        }
        break;
      }
      if (target.state != MitigationState::Acknowledged &&
          target.state != MitigationState::Observed) {
        return Status::error(StatusCode::RequestStateConflict,
                             "only an acknowledged or observed request can be verified")
            .with_context(std::string(mitigation_state_name(target.state)));
      }
      set_request_state(target, MitigationState::Verified, entry.at, FailureCode::None,
                        SupersedeReason::None, target.transport_code, bounds);
      target.verified_at = entry.at;
      target.has_verified_at = true;
      target.verification_ref = payload.verification_ref;
      target.has_verification_ref = payload.verification_ref.valid();
      break;
    }

    case JournalKind::RequestFailed: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<RequestFailedPayload>(entry.payload);
      auto record = require_request(*incident, payload.request);
      if (!record.ok()) {
        return record.status();
      }
      MitigationRecord& target = *record.value();
      if (target.is_terminal()) {
        return Status::error(StatusCode::RequestStateConflict,
                             "a terminal request cannot fail again")
            .with_context(std::string(mitigation_state_name(target.state)));
      }
      set_request_state(target, MitigationState::Failed, entry.at, payload.failure,
                        SupersedeReason::None, payload.transport_code, bounds);
      ++outcome.requests_failed;
      break;
    }

    case JournalKind::RequestSuperseded: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<RequestSupersededPayload>(entry.payload);
      auto record = require_request(*incident, payload.request);
      if (!record.ok()) {
        return record.status();
      }
      MitigationRecord& target = *record.value();
      if (target.is_terminal()) {
        return Status::error(StatusCode::RequestStateConflict,
                             "a terminal request cannot be superseded")
            .with_context(std::string(mitigation_state_name(target.state)));
      }
      set_request_state(target, MitigationState::Superseded, entry.at, FailureCode::None,
                        payload.reason, target.transport_code, bounds);
      break;
    }

    case JournalKind::RequestAbandoned: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<RequestAbandonedPayload>(entry.payload);
      auto record = require_request(*incident, payload.request);
      if (!record.ok()) {
        return record.status();
      }
      MitigationRecord& target = *record.value();
      if (target.is_terminal()) {
        return Status::error(StatusCode::RequestStateConflict,
                             "a terminal request cannot be abandoned")
            .with_context(std::string(mitigation_state_name(target.state)));
      }
      set_request_state(target, MitigationState::Abandoned, entry.at, FailureCode::None,
                        SupersedeReason::None, target.transport_code, bounds);
      break;
    }

    case JournalKind::ConstraintRelaxed: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident,
                             "constraint relaxation requires an open incident");
      }
      const auto& payload = std::get<ConstraintRelaxedPayload>(entry.payload);
      ObligationSlot* slot = find_obligation(state, payload.obligation);
      if (slot == nullptr) {
        return Status::error(StatusCode::ObligationUnknown, "obligation is not registered");
      }
      bool updated = false;
      for (RelaxationRecord& relaxation : incident->relaxations) {
        if (relaxation.obligation == payload.obligation && relaxation.cls == payload.cls) {
          relaxation.active = true;
          relaxation.granted_by = payload.authority;
          relaxation.granted_at = entry.at;
          updated = true;
          break;
        }
      }
      if (!updated) {
        if (incident->relaxations.size() >= bounds.max_relaxations_per_incident) {
          return Status::error(StatusCode::ResourceExhausted, "relaxation table is full");
        }
        RelaxationRecord relaxation;
        relaxation.obligation = payload.obligation;
        relaxation.cls = payload.cls;
        relaxation.granted_by = payload.authority;
        relaxation.granted_at = entry.at;
        relaxation.active = true;
        incident->relaxations.push_back(std::move(relaxation));
      }
      break;
    }

    case JournalKind::ConstraintReimposed: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      const auto& payload = std::get<ConstraintReimposedPayload>(entry.payload);
      bool found = false;
      for (RelaxationRecord& relaxation : incident->relaxations) {
        if (relaxation.obligation == payload.obligation && relaxation.active) {
          relaxation.active = false;
          found = true;
        }
      }
      if (!found) {
        return Status::error(StatusCode::ObligationNotRelaxed,
                             "no active relaxation exists for the obligation");
      }
      break;
    }

    case JournalKind::RecoveryBegun: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      if (incident->lifecycle != Lifecycle::Stabilizing) {
        return Status::error(StatusCode::IllegalTransition,
                             "recovery can only begin from the stabilizing lifecycle");
      }
      incident->lifecycle = Lifecycle::Recovering;
      incident->began_recovery_at = entry.at;
      incident->has_begun_recovery = true;
      incident->last_deescalation_at = entry.at;
      incident->has_last_deescalation = true;
      record_transition(state, *incident, entry, incident->severity, incident->severity,
                        Lifecycle::Stabilizing, Lifecycle::Recovering,
                        TransitionTrigger::RecoveryBegun, "operator began recovery");
      break;
    }

    case JournalKind::IncidentClosed: {
      if (incident == nullptr) {
        return Status::error(StatusCode::NoActiveIncident, "no open incident");
      }
      if (incident->lifecycle != Lifecycle::Recovered && incident->lifecycle != Lifecycle::Active &&
          incident->lifecycle != Lifecycle::Stabilizing) {
        return Status::error(StatusCode::IllegalTransition,
                             "the incident cannot be closed from its current lifecycle");
      }
      const auto& payload = std::get<IncidentClosedPayload>(entry.payload);
      const Lifecycle from_lifecycle = incident->lifecycle;
      incident->lifecycle = Lifecycle::Closed;
      incident->closed = true;
      incident->closed_at = entry.at;
      incident->disposition = payload.disposition;
      incident->has_disposition = true;
      incident->closed_by = payload.operator_ref;
      incident->recovered_ok = false;
      for (RelaxationRecord& relaxation : incident->relaxations) {
        relaxation.active = false;
      }
      for (MitigationRecord& record : incident->requests) {
        if (!record.is_terminal()) {
          set_request_state(record, MitigationState::Abandoned, entry.at, FailureCode::None,
                            SupersedeReason::SeverityReduced, record.transport_code, bounds);
          ++outcome.requests_abandoned;
        }
      }
      record_transition(state, *incident, entry, incident->severity, incident->severity,
                        from_lifecycle, Lifecycle::Closed, TransitionTrigger::IncidentClosed,
                        std::string("closed as ") +
                            std::string(disposition_name(payload.disposition)));
      // The closed incident stays current so that inspection can still report
      // its identity, severity, and disposition. A later excursion opens a new
      // incident with a new id and generation, because opening is refused while
      // the current incident is open and this one is closed.
      break;
    }

    case JournalKind::AuthorityRolledOver: {
      const auto& payload = std::get<AuthorityRolledOverPayload>(entry.payload);
      if (payload.epoch < state.epoch) {
        return Status::error(StatusCode::StaleEpoch,
                             "authority rollover must advance the control epoch");
      }
      state.epoch = payload.epoch;
      state.incarnation = payload.incarnation;
      state.recovered_from_store = false;
      for (IncidentProjection& other : state.incidents) {
        for (MitigationRecord& record : other.requests) {
          if (!record.is_terminal()) {
            set_request_state(record, MitigationState::Superseded, entry.at, FailureCode::None,
                              SupersedeReason::AuthorityRollover, record.transport_code, bounds);
            ++other.fenced_requests;
          }
        }
        const Lifecycle from_lifecycle = other.lifecycle;
        if (from_lifecycle == Lifecycle::Stabilizing || from_lifecycle == Lifecycle::Recovering ||
            from_lifecycle == Lifecycle::Recovered) {
          other.lifecycle = Lifecycle::Active;
          other.recovered_ok = false;
          record_transition(state, other, entry, other.severity, other.severity, from_lifecycle,
                            Lifecycle::Active, TransitionTrigger::AuthorityRollover,
                            "a new authority must re-establish recovery from current evidence");
        } else if (from_lifecycle == Lifecycle::Active) {
          other.recovered_ok = false;
        }
        other.margin_ok = false;
        other.margin_since = entry.at;
        other.has_begun_recovery = false;
        other.has_last_deescalation = false;
      }
      break;
    }

    case JournalKind::PolicyAdopted: {
      const auto& payload = std::get<PolicyAdoptedPayload>(entry.payload);
      auto validated = validate_policy(payload.policy);
      if (!validated.ok()) {
        return validated.status();
      }
      state.policy = payload.policy;
      state.relaxation = payload.relaxation;
      break;
    }

    case JournalKind::StoreRecovered: {
      const auto& payload = std::get<StoreRecoveredPayload>(entry.payload);
      state.recovered_from_store = true;
      for (ZoneSlot& zone : state.zones) {
        for (ProbeSlot& probe : zone.probes) {
          probe.live_since_recovery = false;
        }
      }
      for (IncidentProjection& other : state.incidents) {
        for (MitigationRecord& record : other.requests) {
          if (record.state == MitigationState::Planned || record.state == MitigationState::Issued) {
            set_request_state(record, MitigationState::Failed, entry.at,
                              FailureCode::IndeterminateDispatch, SupersedeReason::None,
                              record.transport_code, bounds);
            record.recovered_from_store = true;
            ++outcome.requests_failed;
          } else if (!record.is_terminal()) {
            record.recovered_from_store = true;
          }
        }
        if (other.lifecycle == Lifecycle::Stabilizing || other.lifecycle == Lifecycle::Recovering) {
          const Lifecycle from_lifecycle = other.lifecycle;
          other.lifecycle = Lifecycle::Active;
          record_transition(state, other, entry, other.severity, other.severity, from_lifecycle,
                            Lifecycle::Active, TransitionTrigger::AdoptedState,
                            payload.rollover ? "restored state under a new authority epoch"
                                             : "restored state in a new runtime");
        }
        other.margin_ok = false;
        other.recovered_ok = false;
      }
      break;
    }

    case JournalKind::Tick:
      break;  // handled above
  }

  return outcome;
}

Result<DomainState> replay(const DomainState& checkpoint, const std::vector<JournalEntry>& journal) {
  DomainState state = checkpoint;
  for (const JournalEntry& entry : journal) {
    auto result = apply(state, entry);
    if (!result.ok()) {
      return Status::error(StatusCode::ReplayDivergence,
                           "replaying the journal diverged from the persisted state")
          .with_context(result.status().to_string());
    }
  }
  return state;
}

}  // namespace summon::tem
