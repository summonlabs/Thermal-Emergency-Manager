// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/status.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace summon::tem {

void trap(const char* expression, const char* file, int line) {
  std::fprintf(stderr, "tem: fatal contract violation: %s (%s:%d)\n",
               expression != nullptr ? expression : "<null>", file != nullptr ? file : "<null>",
               line);
  std::fflush(stderr);
  std::abort();
}

std::string_view status_code_name(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "tem.ok";
    case StatusCode::InvalidArgument: return "tem.invalid_argument";
    case StatusCode::EmptyField: return "tem.empty_field";
    case StatusCode::FieldTooLong: return "tem.field_too_long";
    case StatusCode::InvalidCharacter: return "tem.invalid_character";
    case StatusCode::ValueOutOfRange: return "tem.value_out_of_range";
    case StatusCode::DuplicateElement: return "tem.duplicate_element";
    case StatusCode::TooManyElements: return "tem.too_many_elements";
    case StatusCode::MalformedEncoding: return "tem.malformed_encoding";
    case StatusCode::ReservedFieldNotZero: return "tem.reserved_field_not_zero";
    case StatusCode::UnknownEnumValue: return "tem.unknown_enum_value";
    case StatusCode::MissingAuthority: return "tem.missing_authority";
    case StatusCode::StaleIncarnation: return "tem.stale_incarnation";
    case StatusCode::StaleEpoch: return "tem.stale_epoch";
    case StatusCode::FutureEpoch: return "tem.future_epoch";
    case StatusCode::StaleRevision: return "tem.stale_revision";
    case StatusCode::FutureRevision: return "tem.future_revision";
    case StatusCode::StaleGeneration: return "tem.stale_generation";
    case StatusCode::FutureGeneration: return "tem.future_generation";
    case StatusCode::CrossIncidentAuthority: return "tem.cross_incident_authority";
    case StatusCode::AuthorityFenced: return "tem.authority_fenced";
    case StatusCode::AuthorityRequired: return "tem.authority_required";
    case StatusCode::PreconditionFailed: return "tem.precondition_failed";
    case StatusCode::IllegalTransition: return "tem.illegal_transition";
    case StatusCode::NoActiveIncident: return "tem.no_active_incident";
    case StatusCode::IncidentNotFound: return "tem.incident_not_found";
    case StatusCode::IncidentClosed: return "tem.incident_closed";
    case StatusCode::SeverityRegressionRefused: return "tem.severity_regression_refused";
    case StatusCode::RecoveryNotEligible: return "tem.recovery_not_eligible";
    case StatusCode::GateNotSatisfied: return "tem.gate_not_satisfied";
    case StatusCode::EvidenceNotCurrent: return "tem.evidence_not_current";
    case StatusCode::IncidentAlreadyOpen: return "tem.incident_already_open";
    case StatusCode::EvidenceRejected: return "tem.evidence_rejected";
    case StatusCode::EvidenceStale: return "tem.evidence_stale";
    case StatusCode::EvidenceFuture: return "tem.evidence_future";
    case StatusCode::EvidenceOutOfOrder: return "tem.evidence_out_of_order";
    case StatusCode::EvidenceContradictory: return "tem.evidence_contradictory";
    case StatusCode::EvidenceUnavailable: return "tem.evidence_unavailable";
    case StatusCode::EvidenceUnknownTarget: return "tem.evidence_unknown_target";
    case StatusCode::EvidenceDuplicate: return "tem.evidence_duplicate";
    case StatusCode::ProtectedObligationViolation: return "tem.protected_obligation_violation";
    case StatusCode::ObligationNotRelaxable: return "tem.obligation_not_relaxable";
    case StatusCode::ObligationUnknown: return "tem.obligation_unknown";
    case StatusCode::ObligationViolated: return "tem.obligation_violated";
    case StatusCode::ObligationAlreadyRelaxed: return "tem.obligation_already_relaxed";
    case StatusCode::ObligationNotRelaxed: return "tem.obligation_not_relaxed";
    case StatusCode::RequestNotFound: return "tem.request_not_found";
    case StatusCode::RequestStateConflict: return "tem.request_state_conflict";
    case StatusCode::RequestExpired: return "tem.request_expired";
    case StatusCode::IdempotencyConflict: return "tem.idempotency_conflict";
    case StatusCode::MitigationUnverified: return "tem.mitigation_unverified";
    case StatusCode::RequestTargetMismatch: return "tem.request_target_mismatch";
    case StatusCode::MitigationFailed: return "tem.mitigation_failed";
    case StatusCode::TransportFailure: return "tem.transport_failure";
    case StatusCode::ResourceExhausted: return "tem.resource_exhausted";
    case StatusCode::BoundsExceeded: return "tem.bounds_exceeded";
    case StatusCode::CheckpointUnavailable: return "tem.checkpoint_unavailable";
    case StatusCode::StoreNotFound: return "tem.store_not_found";
    case StatusCode::StoreCorrupt: return "tem.store_corrupt";
    case StatusCode::StoreVersionUnsupported: return "tem.store_version_unsupported";
    case StatusCode::StoreTruncated: return "tem.store_truncated";
    case StatusCode::StoreTrailingBytes: return "tem.store_trailing_bytes";
    case StatusCode::StoreIntegrityFailure: return "tem.store_integrity_failure";
    case StatusCode::StoreLocked: return "tem.store_locked";
    case StatusCode::StorePathInvalid: return "tem.store_path_invalid";
    case StatusCode::StoreIoError: return "tem.store_io_error";
    case StatusCode::StoreReadbackMismatch: return "tem.store_readback_mismatch";
    case StatusCode::NoAuthoritativeGeneration: return "tem.no_authoritative_generation";
    case StatusCode::ReplayDivergence: return "tem.replay_divergence";
    case StatusCode::StoreAlreadyExists: return "tem.store_already_exists";
    case StatusCode::RuntimeClosed: return "tem.runtime_closed";
    case StatusCode::ShutdownInProgress: return "tem.shutdown_in_progress";
    case StatusCode::ReentrancyRefused: return "tem.reentrancy_refused";
    case StatusCode::NotSupported: return "tem.not_supported";
    case StatusCode::Internal: return "tem.internal";
    case StatusCode::ConcurrencyConflict: return "tem.concurrency_conflict";
  }
  return "tem.unknown";
}

std::optional<StatusCode> status_code_from_value(std::uint16_t value) noexcept {
  const auto code = static_cast<StatusCode>(value);
  if (status_code_name(code) == std::string_view{"tem.unknown"}) {
    return std::nullopt;
  }
  return code;
}

std::string Status::to_string() const {
  std::string out(code_name());
  out.append(": ");
  out.append(message_);
  if (!context_.empty()) {
    out.append(" [");
    out.append(context_);
    out.push_back(']');
  }
  return out;
}

}  // namespace summon::tem
