// Thermal Emergency Manager -- policy thresholds, hysteresis, and resource bounds.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>

#include "tem/ids.hpp"
#include "tem/severity.hpp"
#include "tem/status.hpp"
#include "tem/units.hpp"

namespace summon::tem {

// Hard upper limits. These are the absolute ceilings that validation can never
// be configured past: declared sizes from untrusted input are checked against
// these before any allocation, not against the configured bounds alone.
struct HardLimits {
  static constexpr std::uint32_t kMaxZones = 4096;
  static constexpr std::uint32_t kMaxProbesPerZone = 64;
  static constexpr std::uint32_t kMaxObligations = 4096;
  static constexpr std::uint32_t kMaxRequestsPerIncident = 8192;
  static constexpr std::uint32_t kMaxRequestHistory = 64;
  static constexpr std::uint32_t kMaxEvidenceRefs = 64;
  static constexpr std::uint32_t kMaxJournalEntries = 262144;
  static constexpr std::uint32_t kMaxIncidents = 1024;
  static constexpr std::uint32_t kMaxTransitionsPerIncident = 4096;
  static constexpr std::uint32_t kMaxRelaxationsPerIncident = 1024;
  static constexpr std::uint32_t kMaxDescriptionLength = 512;
  static constexpr std::uint32_t kMaxRequestsPerIssue = 64;
  static constexpr std::uint64_t kMaxSnapshotBytes = 128ull * 1024ull * 1024ull;
  static constexpr std::uint32_t kMaxZonesPerAssessment = 4096;
};

// Configured resource bounds. Defaults are deliberately small: a thermal
// emergency is a bounded-scope incident, and an unbounded audit or request
// table is a defect rather than a feature.
struct Bounds {
  std::uint32_t max_zones = 128;
  std::uint32_t max_probes_per_zone = 8;
  std::uint32_t max_obligations = 256;
  std::uint32_t max_requests_per_incident = 512;
  std::uint32_t max_request_history = 8;
  std::uint32_t max_evidence_refs = 16;
  std::uint32_t max_journal_entries = 4096;
  std::uint32_t max_incidents = 32;
  std::uint32_t max_transitions_per_incident = 512;
  std::uint32_t max_relaxations_per_incident = 256;
  std::uint32_t max_description_length = 192;
  std::uint32_t max_requests_per_issue = 32;
};

// Thermal policy. All temperatures are millidegrees Celsius, all rates are
// millidegrees Celsius per minute, and all dwells are exact nanosecond
// durations.
struct ThermalPolicy {
  PolicyGeneration generation{PolicyGeneration::from_value(1)};

  // Escalation thresholds. Strictly increasing.
  MilliCelsius advisory_enter = MilliCelsius::from_value(27'000);      // 27.000 C
  MilliCelsius warning_enter = MilliCelsius::from_value(32'000);       // 32.000 C
  MilliCelsius critical_enter = MilliCelsius::from_value(38'000);      // 38.000 C
  MilliCelsius emergency_enter = MilliCelsius::from_value(45'000);     // 45.000 C
  MilliCelsius catastrophic_enter = MilliCelsius::from_value(55'000);  // 55.000 C

  // Rate floors. A sustained rise is itself an emergency signal even before a
  // static threshold is crossed.
  MilliCelsiusPerMinute rapid_rate = MilliCelsiusPerMinute::from_value(2'000);     // 2.000 C/min
  MilliCelsiusPerMinute extreme_rate = MilliCelsiusPerMinute::from_value(5'000);   // 5.000 C/min
  MilliCelsius rapid_rate_floor = MilliCelsius::from_value(32'000);    // rate rule needs >= warning
  MilliCelsius extreme_rate_floor = MilliCelsius::from_value(27'000);  // rate rule needs >= advisory

  // Hysteresis: recovery from a level requires dropping below that level's
  // enter threshold minus this margin. Must be > 0.
  MilliCelsius recovery_margin = MilliCelsius::from_value(2'000);

  // Continuous time the margin condition must hold before a recovery step is
  // taken, and the minimum spacing between two recovery steps.
  Duration recovery_dwell = Duration::from_seconds(120);
  Duration deescalation_dwell = Duration::from_seconds(60);
  Duration closure_dwell = Duration::from_seconds(60);

  // Evidence currency.
  Duration evidence_staleness = Duration::from_seconds(30);
  Duration evidence_future_skew = Duration::from_seconds(5);
  MilliCelsius contradiction_tolerance = MilliCelsius::from_value(5'000);

  // Mitigation request validity and verification windows.
  Duration request_validity = Duration::from_minutes(5);
  Duration verification_deadline = Duration::from_minutes(3);
  Duration verification_validity = Duration::from_minutes(10);

  // When current evidence is lost during an active incident, the severity
  // floor is raised to Warning and de-escalation is blocked until current
  // evidence is restored.
  bool escalate_on_evidence_loss = true;

  Bounds bounds{};
};

// Validates a policy. Precedence (first failure wins):
//   1. zero policy generation                              -> ValueOutOfRange
//   2. thresholds not strictly increasing                  -> ValueOutOfRange
//   3. negative rates or rate floors above their threshold -> ValueOutOfRange
//   4. recovery margin zero or not below the level gap     -> ValueOutOfRange
//   5. non-positive or non-monotonic dwells                -> ValueOutOfRange
//   6. non-positive evidence windows                       -> ValueOutOfRange
//   7. negative contradiction tolerance                    -> ValueOutOfRange
//   8. bounds zero, above hard limits, or inconsistent     -> BoundsExceeded
[[nodiscard]] Result<ThermalPolicy> validate_policy(const ThermalPolicy& policy);

// Threshold that escalates to a given severity, and the threshold below which
// recovery from that severity is permitted (enter - margin).
[[nodiscard]] MilliCelsius severity_enter_threshold(Severity, const ThermalPolicy&);
[[nodiscard]] Result<MilliCelsius> severity_recovery_threshold(Severity, const ThermalPolicy&);

}  // namespace summon::tem
