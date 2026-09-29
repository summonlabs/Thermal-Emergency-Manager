// Thermal Emergency Manager -- deterministic escalation planning.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <vector>

#include "tem/evidence.hpp"
#include "tem/mitigation.hpp"
#include "tem/recovery.hpp"
#include "tem/severity.hpp"
#include "tem/state.hpp"

namespace summon::tem {

// A request the plan says must exist for the current severity and evidence.
struct PlannedRequest {
  MitigationClass cls{MitigationClass::DerateAccelerators};
  RequestTarget target{};
  BasisPoints intensity{};
  Severity trigger_severity{Severity::Nominal};
  RequestReason reason{RequestReason::EscalationSeverity};
  std::vector<RefToken> evidence_refs{};
};

// The complete answer to "which bounded cross-domain mitigations must be
// requested now". The plan is a pure function of the state and the instant, so
// two evaluations over the same evidence produce the same action sequence, in
// the same order.
struct EscalationPlan {
  Severity severity{Severity::Nominal};
  Severity justified{Severity::Nominal};
  Severity floor{Severity::Nominal};
  Lifecycle lifecycle{Lifecycle::None};
  bool evidence_lost{false};
  std::vector<PlannedRequest> requests{};
  std::vector<MitigationRequestId> abandon{};
  Timestamp evaluated_at{};

  [[nodiscard]] bool empty() const noexcept { return requests.empty() && abandon.empty(); }
};

// Fixed, documented intensity table. Values are basis points of the requested
// reduction (10000 = the full scope of the class). The table is deliberately
// not configurable so that the requested action for a given severity is
// reproducible from the audit record alone.
[[nodiscard]] BasisPoints planned_intensity(MitigationClass cls, Severity severity) noexcept;

// Severity justified by a single zone's own evidence.
[[nodiscard]] Severity zone_justified_severity(const ZoneAssessment& zone,
                                               const ThermalPolicy& policy) noexcept;

// Builds the plan. Ordering: request class ascending, then target bytes, then
// the zone reference order of the facility configuration.
[[nodiscard]] EscalationPlan plan_escalation(const DomainState& state, Timestamp now);

// The minimum severity at which a class becomes required.
[[nodiscard]] Severity mitigation_class_min_severity(MitigationClass cls) noexcept;

// True when an outstanding request is still justified by the current severity
// and by the condition of the target it addresses. Used both by the planner and
// by the tick that abandons requests which are no longer required.
[[nodiscard]] bool request_still_required(const DomainState& state,
                                          const IncidentProjection& incident,
                                          const EvidenceAssessment& assessment,
                                          const MitigationRecord& record);

// True when at least one addressable target in the facility currently justifies
// the mitigation class. A required class with no such target is moot: there is
// nothing left for the mitigation to act on, and demanding a verified request
// for it would deadlock recovery against the tick that abandons unneeded
// requests.
[[nodiscard]] bool mitigation_class_justified(const DomainState& state,
                                              const EvidenceAssessment& assessment,
                                              MitigationClass cls);

// Maximum number of attempts the runtime will make for one required
// (class, target) pair before it stops re-issuing and lets the requirement stay
// unmet, which by construction blocks recovery.
inline constexpr std::uint32_t kMaxRequestAttempts = 3;

}  // namespace summon::tem
