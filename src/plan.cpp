// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/plan.hpp"

#include <algorithm>
#include <string>

namespace summon::tem {
namespace {

}  // namespace

Severity mitigation_class_min_severity(MitigationClass cls) noexcept {
  switch (cls) {
    case MitigationClass::DerateAccelerators: return Severity::Warning;
    case MitigationClass::DrainWorkload: return Severity::Critical;
    case MitigationClass::ReducePower: return Severity::Emergency;
    case MitigationClass::IsolateEquipment: return Severity::Catastrophic;
  }
  return Severity::Catastrophic;
}

namespace {

Severity zone_level(const ZoneAssessment& zone, const ThermalPolicy& policy) noexcept {
  if (zone.cls != FreshnessClass::Fresh || !zone.has_temperature) {
    return Severity::Nominal;
  }
  Severity level = Severity::Nominal;
  const MilliCelsius temperature = zone.max_temperature;
  if (temperature >= policy.catastrophic_enter) {
    level = Severity::Catastrophic;
  } else if (temperature >= policy.emergency_enter) {
    level = Severity::Emergency;
  } else if (temperature >= policy.critical_enter) {
    level = Severity::Critical;
  } else if (temperature >= policy.warning_enter) {
    level = Severity::Warning;
  } else if (temperature >= policy.advisory_enter) {
    level = Severity::Advisory;
  }
  if (zone.has_rate) {
    if (zone.peak_rate >= policy.extreme_rate && temperature >= policy.extreme_rate_floor) {
      level = severity_max(level, Severity::Emergency);
    }
    if (zone.peak_rate >= policy.rapid_rate && temperature >= policy.rapid_rate_floor) {
      level = severity_max(level, Severity::Critical);
    }
  }
  return level;
}

const ZoneAssessment* zone_assessment(const EvidenceAssessment& assessment,
                                      const ThermalZoneRef& zone) noexcept {
  for (const ZoneAssessment& candidate : assessment.zones) {
    if (candidate.zone == zone) {
      return &candidate;
    }
  }
  return nullptr;
}

// Resolves the thermal zone a request target belongs to, using the opaque
// facility bindings supplied by the facility authority.
const ThermalZoneRef* zone_of_target(const DomainState& state, const RequestTarget& target) noexcept {
  if (target.kind() == RefKind::ThermalZone) {
    return &target.zone();
  }
  for (const TargetBinding& binding : state.bindings) {
    if (target.kind() == RefKind::PowerDomain && binding.power_domain == target.power_domain()) {
      return &binding.zone;
    }
    if (target.kind() == RefKind::Equipment) {
      for (const EquipmentRef& equipment : binding.equipment) {
        if (equipment == target.equipment()) {
          return &binding.zone;
        }
      }
    }
  }
  return nullptr;
}

bool same_target(const RequestTarget& a, const RequestTarget& b) noexcept { return a == b; }

}  // namespace

BasisPoints planned_intensity(MitigationClass cls, Severity severity) noexcept {
  const std::uint8_t level = severity_ordinal(severity);
  switch (cls) {
    case MitigationClass::DerateAccelerators:
      if (level >= severity_ordinal(Severity::Catastrophic)) return BasisPoints::from_value(9000);
      if (level >= severity_ordinal(Severity::Emergency)) return BasisPoints::from_value(7000);
      if (level >= severity_ordinal(Severity::Critical)) return BasisPoints::from_value(5000);
      if (level >= severity_ordinal(Severity::Warning)) return BasisPoints::from_value(3000);
      return BasisPoints::from_value(0);
    case MitigationClass::DrainWorkload:
      if (level >= severity_ordinal(Severity::Catastrophic)) return BasisPoints::from_value(10000);
      if (level >= severity_ordinal(Severity::Emergency)) return BasisPoints::from_value(5000);
      if (level >= severity_ordinal(Severity::Critical)) return BasisPoints::from_value(2500);
      return BasisPoints::from_value(0);
    case MitigationClass::ReducePower:
      if (level >= severity_ordinal(Severity::Catastrophic)) return BasisPoints::from_value(5000);
      if (level >= severity_ordinal(Severity::Emergency)) return BasisPoints::from_value(2000);
      return BasisPoints::from_value(0);
    case MitigationClass::IsolateEquipment:
      if (level >= severity_ordinal(Severity::Catastrophic)) return BasisPoints::from_value(10000);
      return BasisPoints::from_value(0);
  }
  return BasisPoints::from_value(0);
}

Severity zone_justified_severity(const ZoneAssessment& zone, const ThermalPolicy& policy) noexcept {
  return zone_level(zone, policy);
}

bool request_still_required(const DomainState& state, const IncidentProjection& incident,
                            const EvidenceAssessment& assessment,
                            const MitigationRecord& record) {
  const std::uint8_t required = required_class_mask(incident.severity);
  if ((required & mitigation_class_bit(record.request.cls)) == 0) {
    return false;
  }
  const ThermalZoneRef* zone = zone_of_target(state, record.request.target);
  if (zone == nullptr) {
    // Without a binding the runtime cannot re-derive the target's condition, so
    // it does not abandon an outstanding request it cannot re-justify.
    return true;
  }
  const ZoneAssessment* measured = zone_assessment(assessment, *zone);
  if (measured == nullptr) {
    return true;
  }
  const Severity level = zone_level(*measured, state.policy);
  return !(severity_ordinal(level) <
           severity_ordinal(mitigation_class_min_severity(record.request.cls)));
}

bool mitigation_class_justified(const DomainState& state, const EvidenceAssessment& assessment,
                                MitigationClass cls) {
  const Severity threshold = mitigation_class_min_severity(cls);
  if (cls == MitigationClass::DerateAccelerators || cls == MitigationClass::DrainWorkload) {
    for (const ZoneAssessment& zone : assessment.zones) {
      if (!(severity_ordinal(zone_level(zone, state.policy)) < severity_ordinal(threshold))) {
        return true;
      }
    }
    return false;
  }
  for (const TargetBinding& binding : state.bindings) {
    const ZoneAssessment* measured = zone_assessment(assessment, binding.zone);
    if (measured == nullptr) {
      continue;
    }
    if (severity_ordinal(zone_level(*measured, state.policy)) < severity_ordinal(threshold)) {
      continue;
    }
    if (cls == MitigationClass::ReducePower && binding.power_domain.valid()) {
      return true;
    }
    if (cls == MitigationClass::IsolateEquipment && !binding.equipment.empty()) {
      return true;
    }
  }
  return false;
}

EscalationPlan plan_escalation(const DomainState& state, Timestamp now) {
  EscalationPlan plan;
  plan.evaluated_at = now;
  const IncidentProjection* incident = find_incident(state, state.current_incident);
  const EvidenceAssessment assessment = assess_state(state, now);
  plan.justified = justified_severity(assessment, state.policy);
  if (incident == nullptr) {
    plan.lifecycle = Lifecycle::None;
    plan.severity = Severity::Nominal;
    return plan;
  }
  plan.severity = incident->severity;
  plan.lifecycle = incident->lifecycle;
  const bool evidence_lost = !assessment.all_fresh && !state.zones.empty();
  plan.evidence_lost = evidence_lost;
  plan.floor = (state.policy.escalate_on_evidence_loss && evidence_lost)
                   ? severity_max(incident->severity, Severity::Warning)
                   : incident->severity;

  if (incident->closed) {
    return plan;
  }

  const std::uint8_t required = required_class_mask(incident->severity);
  const std::size_t evidence_bound = state.policy.bounds.max_evidence_refs;

  const auto push_request = [&](MitigationClass cls, const RequestTarget& target,
                                const ThermalZoneRef& evidence_zone) {
    if ((required & mitigation_class_bit(cls)) == 0) {
      return;
    }
    // A protected obligation constrains the plan exactly as it constrains an
    // explicit request: the automatic path never routes around a constraint.
    if (find_blocking_obligation(state, *incident, target, cls) != nullptr) {
      return;
    }
    for (const PlannedRequest& existing : plan.requests) {
      if (existing.cls == cls && same_target(existing.target, target)) {
        return;
      }
    }
    PlannedRequest request;
    request.cls = cls;
    request.target = target;
    request.intensity = planned_intensity(cls, incident->severity);
    request.trigger_severity = incident->severity;
    request.reason = RequestReason::EscalationSeverity;
    if (evidence_bound > 0) {
      request.evidence_refs.push_back(evidence_zone.token());
    }
    plan.requests.push_back(std::move(request));
  };

  // Zone-targeted classes, in facility configuration order.
  for (const ZoneAssessment& zone : assessment.zones) {
    const Severity level = zone_level(zone, state.policy);
    if (!(severity_ordinal(level) < severity_ordinal(Severity::Warning))) {
      push_request(MitigationClass::DerateAccelerators, RequestTarget::for_zone(zone.zone),
                   zone.zone);
    }
    if (!(severity_ordinal(level) < severity_ordinal(Severity::Critical))) {
      push_request(MitigationClass::DrainWorkload, RequestTarget::for_zone(zone.zone), zone.zone);
    }
  }

  // Power-domain and equipment classes follow the registered facility bindings.
  for (const TargetBinding& binding : state.bindings) {
    const ZoneAssessment* measured = zone_assessment(assessment, binding.zone);
    if (measured == nullptr) {
      continue;
    }
    const Severity level = zone_level(*measured, state.policy);
    if (!(severity_ordinal(level) < severity_ordinal(Severity::Emergency)) &&
        binding.power_domain.valid()) {
      push_request(MitigationClass::ReducePower,
                   RequestTarget::for_power_domain(binding.power_domain), binding.zone);
    }
    if (!(severity_ordinal(level) < severity_ordinal(Severity::Catastrophic))) {
      for (const EquipmentRef& equipment : binding.equipment) {
        push_request(MitigationClass::IsolateEquipment, RequestTarget::for_equipment(equipment),
                     binding.zone);
      }
    }
  }

  std::sort(plan.requests.begin(), plan.requests.end(),
            [](const PlannedRequest& a, const PlannedRequest& b) {
              if (mitigation_class_ordinal(a.cls) != mitigation_class_ordinal(b.cls)) {
                return mitigation_class_ordinal(a.cls) < mitigation_class_ordinal(b.cls);
              }
              return a.target < b.target;
            });

  for (const MitigationRecord& record : incident->requests) {
    if (record.is_terminal()) {
      continue;
    }
    if (!request_still_required(state, *incident, assessment, record)) {
      plan.abandon.push_back(record.request.id);
    }
  }
  std::sort(plan.abandon.begin(), plan.abandon.end());
  return plan;
}

}  // namespace summon::tem
