// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/policy.hpp"

#include <cstdint>

namespace summon::tem {
namespace {

Status bounds_error(const char* what) {
  return Status::error(StatusCode::BoundsExceeded, what);
}

}  // namespace

Result<ThermalPolicy> validate_policy(const ThermalPolicy& policy) {
  if (policy.generation.is_absent()) {
    return Status::error(StatusCode::ValueOutOfRange, "policy generation must be non-zero");
  }

  const MilliCelsius ladder[5] = {policy.advisory_enter, policy.warning_enter, policy.critical_enter,
                                  policy.emergency_enter, policy.catastrophic_enter};
  for (std::size_t i = 1; i < 5; ++i) {
    if (!(ladder[i - 1] < ladder[i])) {
      return Status::error(StatusCode::ValueOutOfRange,
                           "escalation thresholds must be strictly increasing");
    }
  }

  if (policy.rapid_rate.value() < 0 || policy.extreme_rate.value() < 0) {
    return Status::error(StatusCode::ValueOutOfRange, "rate thresholds must not be negative");
  }
  if (policy.extreme_rate < policy.rapid_rate) {
    return Status::error(
        StatusCode::ValueOutOfRange,
        "the extreme rate threshold must not be below the rapid rate threshold");
  }
  if (policy.rapid_rate_floor < policy.advisory_enter ||
      policy.extreme_rate_floor < policy.advisory_enter) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "rate floors must not be below the advisory threshold");
  }

  if (policy.recovery_margin.value() <= 0) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "recovery margin must be positive so that hysteresis exists");
  }
  for (std::size_t i = 1; i < 5; ++i) {
    const auto reduced = ladder[i].checked_sub(policy.recovery_margin);
    if (!reduced.has_value()) {
      return Status::error(StatusCode::ValueOutOfRange, "recovery threshold underflow");
    }
    if (*reduced < ladder[i - 1]) {
      return Status::error(
          StatusCode::ValueOutOfRange,
          "recovery hysteresis band must not reach below the next lower escalation threshold");
    }
  }

  if (policy.recovery_dwell.nanos() <= 0 || policy.deescalation_dwell.nanos() <= 0 ||
      policy.closure_dwell.nanos() <= 0) {
    return Status::error(StatusCode::ValueOutOfRange, "dwell durations must be positive");
  }
  if (policy.evidence_staleness.nanos() <= 0) {
    return Status::error(StatusCode::ValueOutOfRange, "evidence staleness window must be positive");
  }
  if (policy.evidence_future_skew.nanos() < 0) {
    return Status::error(StatusCode::ValueOutOfRange, "evidence future skew must not be negative");
  }
  if (policy.contradiction_tolerance.value() < 0) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "contradiction tolerance must not be negative");
  }
  if (policy.request_validity.nanos() <= 0 || policy.verification_deadline.nanos() <= 0 ||
      policy.verification_validity.nanos() <= 0) {
    return Status::error(StatusCode::ValueOutOfRange,
                         "request validity and verification windows must be positive");
  }

  const Bounds& b = policy.bounds;
  if (b.max_zones == 0 || b.max_zones > HardLimits::kMaxZones) {
    return bounds_error("max_zones is zero or above the hard limit");
  }
  if (b.max_probes_per_zone == 0 || b.max_probes_per_zone > HardLimits::kMaxProbesPerZone) {
    return bounds_error("max_probes_per_zone is zero or above the hard limit");
  }
  if (b.max_obligations == 0 || b.max_obligations > HardLimits::kMaxObligations) {
    return bounds_error("max_obligations is zero or above the hard limit");
  }
  if (b.max_requests_per_incident == 0 ||
      b.max_requests_per_incident > HardLimits::kMaxRequestsPerIncident) {
    return bounds_error("max_requests_per_incident is zero or above the hard limit");
  }
  if (b.max_request_history == 0 || b.max_request_history > HardLimits::kMaxRequestHistory) {
    return bounds_error("max_request_history is zero or above the hard limit");
  }
  if (b.max_evidence_refs == 0 || b.max_evidence_refs > HardLimits::kMaxEvidenceRefs) {
    return bounds_error("max_evidence_refs is zero or above the hard limit");
  }
  if (b.max_journal_entries == 0 || b.max_journal_entries > HardLimits::kMaxJournalEntries) {
    return bounds_error("max_journal_entries is zero or above the hard limit");
  }
  if (b.max_incidents == 0 || b.max_incidents > HardLimits::kMaxIncidents) {
    return bounds_error("max_incidents is zero or above the hard limit");
  }
  if (b.max_transitions_per_incident == 0 ||
      b.max_transitions_per_incident > HardLimits::kMaxTransitionsPerIncident) {
    return bounds_error("max_transitions_per_incident is zero or above the hard limit");
  }
  if (b.max_relaxations_per_incident == 0 ||
      b.max_relaxations_per_incident > HardLimits::kMaxRelaxationsPerIncident) {
    return bounds_error("max_relaxations_per_incident is zero or above the hard limit");
  }
  if (b.max_description_length == 0 ||
      b.max_description_length > HardLimits::kMaxDescriptionLength) {
    return bounds_error("max_description_length is zero or above the hard limit");
  }
  if (b.max_requests_per_issue == 0 || b.max_requests_per_issue > HardLimits::kMaxRequestsPerIssue) {
    return bounds_error("max_requests_per_issue is zero or above the hard limit");
  }
  if (b.max_requests_per_issue > b.max_requests_per_incident) {
    return bounds_error("max_requests_per_issue exceeds max_requests_per_incident");
  }
  return policy;
}

MilliCelsius severity_enter_threshold(Severity severity, const ThermalPolicy& policy) {
  switch (severity) {
    case Severity::Nominal: return MilliCelsius::from_value(0);
    case Severity::Advisory: return policy.advisory_enter;
    case Severity::Warning: return policy.warning_enter;
    case Severity::Critical: return policy.critical_enter;
    case Severity::Emergency: return policy.emergency_enter;
    case Severity::Catastrophic: return policy.catastrophic_enter;
  }
  return MilliCelsius::from_value(0);
}

Result<MilliCelsius> severity_recovery_threshold(Severity severity, const ThermalPolicy& policy) {
  const MilliCelsius enter = severity_enter_threshold(severity, policy);
  if (severity == Severity::Nominal) {
    return enter;
  }
  const auto reduced = enter.checked_sub(policy.recovery_margin);
  if (!reduced.has_value()) {
    return Status::error(StatusCode::ValueOutOfRange, "recovery threshold underflow");
  }
  return *reduced;
}

}  // namespace summon::tem
