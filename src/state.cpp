// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/state.hpp"

#include <algorithm>
#include <string>

#include "tem/codec.hpp"
#include "tem/plan.hpp"

namespace summon::tem {
namespace {

FreshnessClass quality_class(SampleQuality quality) noexcept {
  switch (quality) {
    case SampleQuality::Good: return FreshnessClass::Fresh;
    case SampleQuality::Suspect: return FreshnessClass::Suspect;
    case SampleQuality::Unavailable: return FreshnessClass::Unavailable;
    case SampleQuality::Unsupported: return FreshnessClass::Unsupported;
    case SampleQuality::Indeterminate: return FreshnessClass::Indeterminate;
  }
  return FreshnessClass::Indeterminate;
}

void merge_gate(GateEvaluation& target, GateResult candidate, std::string detail) {
  const auto rank = [](GateResult result) {
    switch (result) {
      case GateResult::Passed: return 0;
      case GateResult::NotApplicable: return 1;
      case GateResult::Unknown: return 2;
      case GateResult::Failed: return 3;
    }
    return 3;
  };
  if (rank(candidate) > rank(target.result)) {
    target.result = candidate;
    target.detail = std::move(detail);
  }
}

std::string describe_zone(const ZoneAssessment& zone) {
  std::string out(zone.zone.text());
  out.push_back('[');
  out.append(freshness_class_name(zone.cls));
  if (zone.has_temperature) {
    out.push_back(' ');
    out.append(format_temperature(zone.max_temperature));
  }
  out.push_back(']');
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

IncidentProjection* find_incident(DomainState& state, IncidentId id) noexcept {
  if (id.is_absent()) {
    return nullptr;
  }
  for (IncidentProjection& incident : state.incidents) {
    if (incident.id == id) {
      return &incident;
    }
  }
  return nullptr;
}

const IncidentProjection* find_incident(const DomainState& state, IncidentId id) noexcept {
  if (id.is_absent()) {
    return nullptr;
  }
  for (const IncidentProjection& incident : state.incidents) {
    if (incident.id == id) {
      return &incident;
    }
  }
  return nullptr;
}

ZoneSlot* find_zone(DomainState& state, const ThermalZoneRef& zone) noexcept {
  for (ZoneSlot& slot : state.zones) {
    if (slot.zone == zone) {
      return &slot;
    }
  }
  return nullptr;
}

const ZoneSlot* find_zone(const DomainState& state, const ThermalZoneRef& zone) noexcept {
  for (const ZoneSlot& slot : state.zones) {
    if (slot.zone == zone) {
      return &slot;
    }
  }
  return nullptr;
}

ProbeSlot* find_probe(ZoneSlot& zone, const ThermalProbeRef& probe) noexcept {
  for (ProbeSlot& slot : zone.probes) {
    if (slot.probe == probe) {
      return &slot;
    }
  }
  return nullptr;
}

const ProbeSlot* find_probe(const ZoneSlot& zone, const ThermalProbeRef& probe) noexcept {
  for (const ProbeSlot& slot : zone.probes) {
    if (slot.probe == probe) {
      return &slot;
    }
  }
  return nullptr;
}

MitigationRecord* find_request(IncidentProjection& incident, MitigationRequestId id) noexcept {
  for (MitigationRecord& record : incident.requests) {
    if (record.request.id == id) {
      return &record;
    }
  }
  return nullptr;
}

const MitigationRecord* find_request(const IncidentProjection& incident,
                                     MitigationRequestId id) noexcept {
  for (const MitigationRecord& record : incident.requests) {
    if (record.request.id == id) {
      return &record;
    }
  }
  return nullptr;
}

MitigationRecord* find_request_by_key(IncidentProjection& incident, IdempotencyKey key) noexcept {
  for (MitigationRecord& record : incident.requests) {
    if (record.request.key == key) {
      return &record;
    }
  }
  return nullptr;
}

const MitigationRecord* find_request_by_key(const IncidentProjection& incident,
                                            IdempotencyKey key) noexcept {
  for (const MitigationRecord& record : incident.requests) {
    if (record.request.key == key) {
      return &record;
    }
  }
  return nullptr;
}

ObligationSlot* find_obligation(DomainState& state, ObligationId id) noexcept {
  for (ObligationSlot& slot : state.obligations) {
    if (slot.obligation.id == id) {
      return &slot;
    }
  }
  return nullptr;
}

const ObligationSlot* find_obligation(const DomainState& state, ObligationId id) noexcept {
  for (const ObligationSlot& slot : state.obligations) {
    if (slot.obligation.id == id) {
      return &slot;
    }
  }
  return nullptr;
}

TargetBinding* find_binding(DomainState& state, const ThermalZoneRef& zone) noexcept {
  for (TargetBinding& binding : state.bindings) {
    if (binding.zone == zone) {
      return &binding;
    }
  }
  return nullptr;
}

const TargetBinding* find_binding(const DomainState& state, const ThermalZoneRef& zone) noexcept {
  for (const TargetBinding& binding : state.bindings) {
    if (binding.zone == zone) {
      return &binding;
    }
  }
  return nullptr;
}

std::vector<const MitigationRecord*> ordered_requests(const IncidentProjection& incident) {
  std::vector<const MitigationRecord*> out;
  out.reserve(incident.requests.size());
  for (const MitigationRecord& record : incident.requests) {
    out.push_back(&record);
  }
  std::sort(out.begin(), out.end(), [](const MitigationRecord* a, const MitigationRecord* b) {
    return order_key_of(a->request) < order_key_of(b->request);
  });
  return out;
}

const ProtectedObligation* find_blocking_obligation(const DomainState& state,
                                                    const IncidentProjection& incident,
                                                    const RequestTarget& target,
                                                    MitigationClass cls) {
  for (const ObligationSlot& slot : state.obligations) {
    const ProtectedObligation& obligation = slot.obligation;
    if (!obligation_forbids(obligation, cls)) {
      continue;
    }
    if (!obligation_covers(obligation, target)) {
      continue;
    }
    bool relaxed = false;
    for (const RelaxationRecord& relaxation : incident.relaxations) {
      if (relaxation.active && relaxation.obligation == obligation.id && relaxation.cls == cls) {
        relaxed = true;
        break;
      }
    }
    if (relaxed) {
      continue;
    }
    return &obligation;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Bounds validation
// ---------------------------------------------------------------------------

Status validate_state_limits(const DomainState& state) {
  const Bounds& bounds = state.policy.bounds;
  if (state.zones.size() > bounds.max_zones) {
    return Status::error(StatusCode::BoundsExceeded, "decoded state exceeds max_zones");
  }
  if (state.obligations.size() > bounds.max_obligations) {
    return Status::error(StatusCode::BoundsExceeded, "decoded state exceeds max_obligations");
  }
  if (state.bindings.size() > bounds.max_zones) {
    return Status::error(StatusCode::BoundsExceeded, "decoded state exceeds the binding bound");
  }
  for (const ZoneSlot& zone : state.zones) {
    if (zone.probes.size() > bounds.max_probes_per_zone) {
      return Status::error(StatusCode::BoundsExceeded, "decoded state exceeds max_probes_per_zone")
          .with_context(zone.zone.text());
    }
  }
  if (state.incidents.size() > bounds.max_incidents) {
    return Status::error(StatusCode::BoundsExceeded, "decoded state exceeds max_incidents");
  }
  for (const IncidentProjection& incident : state.incidents) {
    if (incident.requests.size() > bounds.max_requests_per_incident) {
      return Status::error(StatusCode::BoundsExceeded,
                           "decoded incident exceeds max_requests_per_incident");
    }
    if (incident.relaxations.size() > bounds.max_relaxations_per_incident) {
      return Status::error(StatusCode::BoundsExceeded,
                           "decoded incident exceeds max_relaxations_per_incident");
    }
    if (incident.transitions.size() > bounds.max_transitions_per_incident) {
      return Status::error(StatusCode::BoundsExceeded,
                           "decoded incident exceeds max_transitions_per_incident");
    }
    for (const MitigationRecord& record : incident.requests) {
      if (record.history.size() > bounds.max_request_history) {
        return Status::error(StatusCode::BoundsExceeded,
                             "decoded request exceeds max_request_history");
      }
      if (record.request.evidence_refs.size() > bounds.max_evidence_refs) {
        return Status::error(StatusCode::BoundsExceeded,
                             "decoded request exceeds max_evidence_refs");
      }
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Evidence assessment
// ---------------------------------------------------------------------------

EvidenceAssessment assess_state(const DomainState& state, Timestamp now) {
  EvidenceAssessment assessment;
  assessment.evaluated_at = now;
  assessment.worst = FreshnessClass::Fresh;
  assessment.all_fresh = true;

  const ThermalPolicy& policy = state.policy;
  for (const ZoneSlot& zone : state.zones) {
    ZoneAssessment result;
    result.zone = zone.zone;
    result.total_probes = static_cast<std::uint32_t>(zone.probes.size());

    FreshnessClass zone_class = FreshnessClass::Fresh;
    bool has_any = false;
    bool has_temperature = false;
    MilliCelsius max_temperature{};
    MilliCelsius min_temperature{};
    MilliCelsiusPerMinute peak_rate{};
    bool has_rate = false;
    std::uint32_t temperature_probes = 0;

    for (const ProbeSlot& probe : zone.probes) {
      FreshnessClass cls = FreshnessClass::Fresh;
      if (!probe.present) {
        cls = FreshnessClass::Absent;
      } else {
        if (probe.sample.quality != SampleQuality::Good) {
          cls = freshness_worst(cls, quality_class(probe.sample.quality));
        }
        if (probe.sample.origin != SampleOrigin::Live || !probe.live_since_recovery) {
          cls = freshness_worst(cls, FreshnessClass::Stale);
        }
        const std::int64_t age_nanos = now.unix_nanos() - probe.sample.observed_at.unix_nanos();
        if (age_nanos < 0) {
          const std::int64_t skew = -age_nanos;
          if (skew > policy.evidence_future_skew.nanos()) {
            cls = freshness_worst(cls, FreshnessClass::Future);
          }
        } else if (age_nanos > policy.evidence_staleness.nanos()) {
          cls = freshness_worst(cls, FreshnessClass::Stale);
        }
        if (cls != FreshnessClass::Absent) {
          has_any = true;
        }
        if (probe.sample.origin == SampleOrigin::Live && probe.live_since_recovery) {
          ++result.live_probes;
        }
      }

      if (cls == FreshnessClass::Fresh) {
        ++result.fresh_probes;
        if (probe.sample.has_temperature) {
          if (!has_temperature) {
            max_temperature = probe.sample.temperature;
            min_temperature = probe.sample.temperature;
            has_temperature = true;
          } else {
            max_temperature = max_temperature < probe.sample.temperature ? probe.sample.temperature
                                                                        : max_temperature;
            min_temperature = min_temperature < probe.sample.temperature ? min_temperature
                                                                        : probe.sample.temperature;
          }
          ++temperature_probes;
        }
        if (probe.sample.has_rate) {
          if (!has_rate || peak_rate < probe.sample.rate) {
            peak_rate = probe.sample.rate;
          }
          has_rate = true;
        }
      }
      zone_class = freshness_worst(zone_class, cls);
    }

    if (zone.probes.empty()) {
      zone_class = FreshnessClass::Absent;
    }
    if (temperature_probes >= 2 && has_temperature) {
      const auto spread = max_temperature.checked_sub(min_temperature);
      if (spread.has_value() && *spread > policy.contradiction_tolerance) {
        zone_class = FreshnessClass::Contradictory;
      }
    }

    result.cls = zone_class;
    result.has_temperature = has_temperature;
    result.max_temperature = max_temperature;
    result.has_rate = has_rate;
    result.peak_rate = peak_rate;
    if (has_any) {
      ++assessment.zones_with_evidence;
      assessment.any_evidence = true;
    }
    if (zone_class != FreshnessClass::Fresh) {
      assessment.all_fresh = false;
    }
    assessment.worst = freshness_worst(assessment.worst, zone_class);
    assessment.zones.push_back(result);
  }

  if (state.zones.empty()) {
    assessment.all_fresh = true;
  }
  return assessment;
}

// ---------------------------------------------------------------------------
// Recovery evaluation
// ---------------------------------------------------------------------------

namespace {

bool margin_condition_holds(const DomainState& state, const EvidenceAssessment& assessment,
                            Severity severity) {
  if (severity == Severity::Nominal) {
    return true;
  }
  auto threshold = severity_recovery_threshold(severity, state.policy);
  if (!threshold.ok()) {
    return false;
  }
  if (assessment.zones.empty()) {
    return false;
  }
  for (const ZoneAssessment& zone : assessment.zones) {
    if (zone.cls != FreshnessClass::Fresh || !zone.has_temperature) {
      return false;
    }
    if (threshold.value() < zone.max_temperature) {
      return false;
    }
  }
  return true;
}

GateEvaluation gate(RecoveryGate which) {
  GateEvaluation evaluation;
  evaluation.gate = which;
  evaluation.result = GateResult::NotApplicable;
  return evaluation;
}

}  // namespace

RecoveryEligibility evaluate_recovery(const DomainState& state, Timestamp now, bool for_closure) {
  RecoveryEligibility eligibility;
  eligibility.evaluated_at = now;
  eligibility.evaluated_revision = state.revision;

  const IncidentProjection* incident = find_incident(state, state.current_incident);
  if (incident == nullptr || incident->lifecycle == Lifecycle::None) {
    eligibility.complete = false;
    eligibility.current_severity = Severity::Nominal;
    eligibility.permitted_severity = Severity::Nominal;
    eligibility.justified = Severity::Nominal;
    return eligibility;
  }

  const EvidenceAssessment assessment = assess_state(state, now);
  const Severity justified = justified_severity(assessment, state.policy);
  eligibility.current_severity = incident->severity;
  eligibility.justified = justified;

  std::vector<GateEvaluation> gates;
  gates.reserve(kRecoveryGateCount);

  // 1. Evidence freshness.
  {
    GateEvaluation evaluation = gate(RecoveryGate::EvidenceFreshness);
    if (state.zones.empty()) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no monitored zones";
    } else {
      evaluation.result = GateResult::Passed;
      for (const ZoneSlot& zone : state.zones) {
        for (const ProbeSlot& probe : zone.probes) {
          const bool fresh = probe.present && probe.live_since_recovery &&
                             probe.sample.origin == SampleOrigin::Live &&
                             probe.sample.quality == SampleQuality::Good;
          const std::int64_t age = now.unix_nanos() - probe.sample.observed_at.unix_nanos();
          if (!probe.present) {
            merge_gate(evaluation, GateResult::Unknown,
                       std::string(probe.probe.text()) + " never reported");
          } else if (!fresh) {
            if (probe.sample.quality != SampleQuality::Good) {
              const FreshnessClass cls = quality_class(probe.sample.quality);
              const GateResult result = (cls == FreshnessClass::Suspect)
                                            ? GateResult::Failed
                                            : GateResult::Unknown;
              merge_gate(evaluation, result,
                         std::string(probe.probe.text()) + " quality " +
                             std::string(sample_quality_name(probe.sample.quality)));
            } else if (probe.sample.origin != SampleOrigin::Live || !probe.live_since_recovery) {
              merge_gate(evaluation, GateResult::Failed,
                         std::string(probe.probe.text()) + " is recovered evidence");
            }
          }
          if (age > state.policy.evidence_staleness.nanos()) {
            merge_gate(evaluation, GateResult::Failed,
                       std::string(probe.probe.text()) + " reading is stale");
          } else if (age < -state.policy.evidence_future_skew.nanos()) {
            merge_gate(evaluation, GateResult::Failed,
                       std::string(probe.probe.text()) + " reading is in the future");
          }
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 2. Evidence completeness.
  {
    GateEvaluation evaluation = gate(RecoveryGate::EvidenceCompleteness);
    if (state.zones.empty()) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no monitored zones";
    } else {
      evaluation.result = GateResult::Passed;
      for (const ZoneSlot& zone : state.zones) {
        if (zone.probes.empty()) {
          continue;
        }
        bool any = false;
        for (const ProbeSlot& probe : zone.probes) {
          if (probe.present) {
            any = true;
            break;
          }
        }
        if (!any) {
          merge_gate(evaluation, GateResult::Unknown,
                     std::string(zone.zone.text()) + " has no reporting probe");
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 3. Evidence consistency.
  {
    GateEvaluation evaluation = gate(RecoveryGate::EvidenceConsistency);
    if (state.zones.empty()) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no monitored zones";
    } else {
      evaluation.result = GateResult::Passed;
      for (const ZoneAssessment& zone : assessment.zones) {
        if (zone.cls == FreshnessClass::Contradictory) {
          merge_gate(evaluation, GateResult::Failed,
                     std::string(zone.zone.text()) + " probes disagree beyond tolerance");
        }
      }
      for (const ZoneSlot& zone : state.zones) {
        for (const ProbeSlot& probe : zone.probes) {
          if (!probe.present) {
            continue;
          }
          if (probe.sample.health == SensorHealth::Failed) {
            merge_gate(evaluation, GateResult::Failed,
                       std::string(probe.probe.text()) + " reports failed health");
          } else if (probe.sample.health == SensorHealth::Degraded) {
            merge_gate(evaluation, GateResult::Failed,
                       std::string(probe.probe.text()) + " reports degraded health");
          } else if (probe.sample.health == SensorHealth::Unknown) {
            merge_gate(evaluation, GateResult::Unknown,
                       std::string(probe.probe.text()) + " health is unknown");
          }
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 4. Thermal margin against the level being left.
  {
    GateEvaluation evaluation = gate(RecoveryGate::ThermalMargin);
    if (incident->severity == Severity::Nominal) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "severity is already nominal";
    } else {
      evaluation.result = GateResult::Passed;
      const auto threshold = severity_recovery_threshold(incident->severity, state.policy);
      if (!threshold.ok()) {
        evaluation.result = GateResult::Unknown;
        evaluation.detail = "recovery threshold is not computable";
      } else {
        for (const ZoneAssessment& zone : assessment.zones) {
          if (zone.cls != FreshnessClass::Fresh) {
            merge_gate(evaluation, GateResult::Unknown, describe_zone(zone) + " not fresh");
            continue;
          }
          if (!zone.has_temperature) {
            merge_gate(evaluation, GateResult::Unknown, describe_zone(zone) + " has no reading");
            continue;
          }
          if (threshold.value() < zone.max_temperature) {
            merge_gate(evaluation, GateResult::Failed,
                       describe_zone(zone) + " above " + format_temperature(threshold.value()));
          }
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 5. Rate margin.
  {
    GateEvaluation evaluation = gate(RecoveryGate::RateMargin);
    bool any_rate_source = false;
    for (const ZoneSlot& zone : state.zones) {
      for (const ProbeSlot& probe : zone.probes) {
        if (probe.present && probe.sample.has_rate) {
          any_rate_source = true;
        }
      }
    }
    if (!any_rate_source) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no rate reporting source";
    } else {
      evaluation.result = GateResult::Passed;
      for (const ZoneAssessment& zone : assessment.zones) {
        if (zone.cls != FreshnessClass::Fresh) {
          continue;
        }
        if (!zone.has_rate) {
          continue;
        }
        if (!(zone.peak_rate < state.policy.rapid_rate)) {
          merge_gate(evaluation, GateResult::Failed,
                     describe_zone(zone) + " rising at " + format_rate(zone.peak_rate));
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 6. Mitigation resolution for the level being left.
  std::vector<MitigationRequestId> verified_requests;
  {
    GateEvaluation evaluation = gate(RecoveryGate::MitigationResolution);
    const std::uint8_t required = required_class_mask(incident->severity);
    if (required == 0) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no mitigation class required at this severity";
    } else {
      evaluation.result = GateResult::Passed;
      const std::array<MitigationClass, kMitigationClassCount> classes = {
          MitigationClass::DerateAccelerators, MitigationClass::DrainWorkload,
          MitigationClass::ReducePower, MitigationClass::IsolateEquipment};
      for (const MitigationClass cls : classes) {
        if ((required & mitigation_class_bit(cls)) == 0) {
          continue;
        }
        bool verified = false;
        bool open = false;
        bool exists = false;
        for (const MitigationRecord& record : incident->requests) {
          if (record.request.cls != cls) {
            continue;
          }
          exists = true;
          if (record.state == MitigationState::Verified) {
            verified = true;
            verified_requests.push_back(record.request.id);
          } else if (!record.is_terminal()) {
            open = true;
          }
        }
        if (verified) {
          continue;
        }
        if (!mitigation_class_justified(state, assessment, cls)) {
          // Nothing addressable still justifies the class: the requirement is
          // moot under current evidence rather than unmet.
          continue;
        }
        if (open) {
          merge_gate(evaluation, GateResult::Failed,
                     std::string(mitigation_class_name(cls)) + " is still in progress");
        } else if (exists) {
          merge_gate(evaluation, GateResult::Failed,
                     std::string(mitigation_class_name(cls)) + " has no verified attempt");
        } else {
          merge_gate(evaluation, GateResult::Unknown,
                     std::string(mitigation_class_name(cls)) + " was never requested");
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 7. Verification currency.
  {
    GateEvaluation evaluation = gate(RecoveryGate::VerificationCurrency);
    if (verified_requests.empty()) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no verified mitigation to age out";
    } else {
      evaluation.result = GateResult::Passed;
      for (const MitigationRequestId id : verified_requests) {
        const MitigationRecord* record = find_request(*incident, id);
        if (record == nullptr || !record->has_verified_at) {
          merge_gate(evaluation, GateResult::Unknown, "verified request has no verification time");
          continue;
        }
        const std::int64_t age = now.unix_nanos() - record->verified_at.unix_nanos();
        if (age < 0 || age > state.policy.verification_validity.nanos()) {
          merge_gate(evaluation, GateResult::Failed,
                     "verification of request " + std::to_string(id.value()) + " is not current");
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 8. Dwell.
  {
    GateEvaluation evaluation = gate(RecoveryGate::Dwell);
    if (incident->severity == Severity::Nominal) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "severity is already nominal";
    } else if (!incident->margin_ok) {
      evaluation.result = GateResult::Failed;
      evaluation.detail = "hysteresis margin is not currently satisfied";
    } else {
      const std::int64_t held = now.unix_nanos() - incident->margin_since.unix_nanos();
      if (held >= state.policy.recovery_dwell.nanos()) {
        evaluation.result = GateResult::Passed;
      } else {
        evaluation.result = GateResult::Failed;
        evaluation.detail = "margin held for " +
                            format_duration(Duration::from_nanos(held < 0 ? 0 : held)) +
                            " of " + format_duration(state.policy.recovery_dwell);
      }
    }
    gates.push_back(evaluation);
  }

  // 9. Interlock integrity.
  {
    GateEvaluation evaluation = gate(RecoveryGate::InterlockIntegrity);
    if (state.obligations.empty()) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "no protected obligations registered";
    } else {
      evaluation.result = GateResult::Passed;
      for (const ObligationSlot& slot : state.obligations) {
        const ProtectedObligation& obligation = slot.obligation;
        bool relaxed = false;
        for (const RelaxationRecord& relaxation : incident->relaxations) {
          if (relaxation.active && relaxation.obligation == obligation.id) {
            relaxed = true;
            break;
          }
        }
        if (relaxed && obligation.protection == ProtectionClass::AdvisoryOptimization) {
          continue;
        }
        if (relaxed && obligation.protection != ProtectionClass::AdvisoryOptimization) {
          merge_gate(evaluation, GateResult::Failed,
                     obligation.ref + " is relaxed but is not an advisory constraint");
          continue;
        }
        if (!obligation.status_present) {
          merge_gate(evaluation, GateResult::Unknown, obligation.ref + " status was never reported");
          continue;
        }
        const std::int64_t age = now.unix_nanos() - obligation.status_at.unix_nanos();
        if (age < 0 || age > state.policy.verification_validity.nanos()) {
          merge_gate(evaluation, GateResult::Unknown,
                     obligation.ref + " status is not current");
          continue;
        }
        switch (obligation.status) {
          case ObligationStatus::Satisfied:
            break;
          case ObligationStatus::Violated:
            merge_gate(evaluation, GateResult::Failed, obligation.ref + " is violated");
            break;
          case ObligationStatus::Unknown:
          case ObligationStatus::Unavailable:
          case ObligationStatus::Unsupported:
            merge_gate(evaluation, GateResult::Unknown,
                       obligation.ref + " status is " +
                           std::string(obligation_status_name(obligation.status)));
            break;
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 10. Authority currency.
  {
    GateEvaluation evaluation = gate(RecoveryGate::AuthorityCurrent);
    evaluation.result = GateResult::Passed;
    for (const IncidentProjection& other : state.incidents) {
      for (const MitigationRecord& record : other.requests) {
        if (!record.is_terminal() && !(record.request.epoch == state.epoch)) {
          merge_gate(evaluation, GateResult::Failed,
                     "request " + std::to_string(record.request.id.value()) +
                         " is bound to a superseded control epoch");
        }
      }
    }
    gates.push_back(evaluation);
  }

  // 11. Closure dwell.
  {
    GateEvaluation evaluation = gate(RecoveryGate::ClosureDwell);
    if (!for_closure) {
      evaluation.result = GateResult::NotApplicable;
      evaluation.detail = "not a closure evaluation";
    } else if (!incident->recovered_ok) {
      evaluation.result = GateResult::Failed;
      evaluation.detail = "recovered state has not held yet";
    } else {
      const std::int64_t held = now.unix_nanos() - incident->recovered_since.unix_nanos();
      if (held >= state.policy.closure_dwell.nanos()) {
        evaluation.result = GateResult::Passed;
      } else {
        evaluation.result = GateResult::Failed;
        evaluation.detail = "recovered for " +
                            format_duration(Duration::from_nanos(held < 0 ? 0 : held)) + " of " +
                            format_duration(state.policy.closure_dwell);
      }
    }
    gates.push_back(evaluation);
  }

  for (const GateEvaluation& evaluation : gates) {
    switch (evaluation.result) {
      case GateResult::Passed: ++eligibility.passed; break;
      case GateResult::Failed: ++eligibility.failed; break;
      case GateResult::Unknown: ++eligibility.unknown; break;
      case GateResult::NotApplicable: ++eligibility.not_applicable; break;
    }
  }
  eligibility.eligible = eligibility.failed == 0 && eligibility.unknown == 0 &&
                         eligibility.passed > 0;
  eligibility.gates = std::move(gates);
  if (eligibility.eligible) {
    const std::uint8_t step = severity_ordinal(incident->severity);
    const Severity reduced = step == 0 ? Severity::Nominal : severity_from_ordinal(step - 1);
    eligibility.permitted_severity = severity_max(reduced, justified);
  } else {
    eligibility.permitted_severity = severity_max(incident->severity, justified);
  }
  return eligibility;
}

// True when every gate other than Dwell and ClosureDwell holds. Used to decide
// whether the incident may enter Stabilizing or stay in Recovering.
bool recovery_preconditions_hold(const RecoveryEligibility& eligibility) {
  for (const GateEvaluation& evaluation : eligibility.gates) {
    if (evaluation.gate == RecoveryGate::Dwell || evaluation.gate == RecoveryGate::ClosureDwell) {
      continue;
    }
    if (evaluation.result == GateResult::Failed || evaluation.result == GateResult::Unknown) {
      return false;
    }
  }
  return true;
}

bool margin_condition(const DomainState& state, const IncidentProjection& incident,
                      const EvidenceAssessment& assessment) {
  return margin_condition_holds(state, assessment, incident.severity);
}

bool states_encode_identically(const DomainState& a, const DomainState& b) {
  const std::vector<std::uint8_t> left = encode_state(a);
  const std::vector<std::uint8_t> right = encode_state(b);
  return left == right;
}

}  // namespace summon::tem
