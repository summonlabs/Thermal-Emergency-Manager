// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/journal.hpp"

namespace summon::tem {

std::string_view journal_kind_name(JournalKind kind) noexcept {
  switch (kind) {
    case JournalKind::IncidentOpened: return "incident-opened";
    case JournalKind::ZoneRegistered: return "zone-registered";
    case JournalKind::ProbeRegistered: return "probe-registered";
    case JournalKind::ObligationRegistered: return "obligation-registered";
    case JournalKind::EvidenceAdmitted: return "evidence-admitted";
    case JournalKind::ObligationStatusReported: return "obligation-status-reported";
    case JournalKind::Tick: return "tick";
    case JournalKind::RequestsIssued: return "requests-issued";
    case JournalKind::RequestAcknowledged: return "request-acknowledged";
    case JournalKind::RequestObserved: return "request-observed";
    case JournalKind::RequestVerified: return "request-verified";
    case JournalKind::RequestFailed: return "request-failed";
    case JournalKind::RequestSuperseded: return "request-superseded";
    case JournalKind::RequestAbandoned: return "request-abandoned";
    case JournalKind::ConstraintRelaxed: return "constraint-relaxed";
    case JournalKind::ConstraintReimposed: return "constraint-reimposed";
    case JournalKind::RecoveryBegun: return "recovery-begun";
    case JournalKind::IncidentClosed: return "incident-closed";
    case JournalKind::AuthorityRolledOver: return "authority-rolled-over";
    case JournalKind::PolicyAdopted: return "policy-adopted";
    case JournalKind::TargetBindingRegistered: return "target-binding-registered";
    case JournalKind::StoreRecovered: return "store-recovered";
  }
  return "unknown";
}

std::optional<JournalKind> journal_kind_from_value(std::uint8_t value) noexcept {
  if (value < 1 || value > kJournalKindCount) {
    return std::nullopt;
  }
  return static_cast<JournalKind>(value);
}

std::string_view transition_trigger_name(TransitionTrigger trigger) noexcept {
  switch (trigger) {
    case TransitionTrigger::IncidentOpened: return "incident-opened";
    case TransitionTrigger::EvidenceEscalation: return "evidence-escalation";
    case TransitionTrigger::RateEscalation: return "rate-escalation";
    case TransitionTrigger::EvidenceLoss: return "evidence-loss";
    case TransitionTrigger::MitigationFailure: return "mitigation-failure";
    case TransitionTrigger::RecoveryStep: return "recovery-step";
    case TransitionTrigger::RecoveryBegun: return "recovery-begun";
    case TransitionTrigger::RecoveryCompleted: return "recovery-completed";
    case TransitionTrigger::AuthorityRollover: return "authority-rollover";
    case TransitionTrigger::IncidentClosed: return "incident-closed";
    case TransitionTrigger::AdoptedState: return "adopted-state";
  }
  return "unknown";
}

std::optional<TransitionTrigger> transition_trigger_from_value(std::uint8_t value) noexcept {
  if (value < 1 || value > 11) {
    return std::nullopt;
  }
  return static_cast<TransitionTrigger>(value);
}

}  // namespace summon::tem
