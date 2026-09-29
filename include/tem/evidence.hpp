// Thermal Emergency Manager -- emergency thermal evidence intake.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "tem/ids.hpp"
#include "tem/policy.hpp"
#include "tem/refs.hpp"
#include "tem/severity.hpp"
#include "tem/status.hpp"
#include "tem/units.hpp"

namespace summon::tem {

// What a sample asserts about the physical world. These are not interchangeable
// with zero: an unavailable probe is not a cold probe.
enum class SampleQuality : std::uint8_t {
  Good = 1,           // a trustworthy reading
  Suspect = 2,        // a reading the producer itself distrusts
  Unavailable = 3,    // the source exists but currently yields no reading
  Unsupported = 4,    // the source cannot express this measurement at all
  Indeterminate = 5,  // the source cannot classify its own output
};

[[nodiscard]] std::string_view sample_quality_name(SampleQuality value) noexcept;
[[nodiscard]] std::optional<SampleQuality> sample_quality_from_value(std::uint8_t value) noexcept;

enum class SensorHealth : std::uint8_t {
  Healthy = 1,
  Degraded = 2,
  Failed = 3,
  Unknown = 4,
};

[[nodiscard]] std::string_view sensor_health_name(SensorHealth value) noexcept;
[[nodiscard]] std::optional<SensorHealth> sensor_health_from_value(std::uint8_t value) noexcept;

// Whether a sample was captured live in this runtime's lifetime or restored
// from durable state. Recovered samples are never treated as current physical
// evidence: they must be replaced by a live sample from the same probe before
// they count towards any gate.
enum class SampleOrigin : std::uint8_t {
  Live = 1,
  Recovered = 2,
};

[[nodiscard]] std::string_view sample_origin_name(SampleOrigin value) noexcept;
[[nodiscard]] std::optional<SampleOrigin> sample_origin_from_value(std::uint8_t value) noexcept;

// One thermal observation from an external producer.
struct ThermalSample {
  ThermalZoneRef zone{};
  ThermalProbeRef probe{};
  SensorSourceRef source{};

  SampleQuality quality{SampleQuality::Good};
  SensorHealth health{SensorHealth::Healthy};
  SampleOrigin origin{SampleOrigin::Live};

  bool has_temperature{true};
  MilliCelsius temperature{};
  bool has_rate{false};
  MilliCelsiusPerMinute rate{};

  Timestamp observed_at{};
  Timestamp received_at{};
  EvidenceGeneration generation{};
  ObservationSequence sequence{};
};

// Validation precedence for admission (first failure wins):
//   1. zone/probe/source tokens malformed or empty            -> InvalidArgument family
//   2. quality/temperature consistency                        -> InvalidArgument
//   3. rate out of range                                      -> ValueOutOfRange
//   4. zone or probe not registered                           -> EvidenceUnknownTarget
//   5. observed_at later than received_at + future skew       -> EvidenceFuture
//   6. sequence not greater than the stored sequence          -> EvidenceOutOfOrder
//      (an identical sequence with identical content)         -> EvidenceDuplicate
[[nodiscard]] Status validate_sample_shape(const ThermalSample& sample, const ThermalPolicy& policy);

// Freshness classification of a probe or zone at an instant.
enum class FreshnessClass : std::uint8_t {
  Fresh = 1,
  Stale = 2,
  Future = 3,
  Absent = 4,
  Suspect = 5,
  Unavailable = 6,
  Indeterminate = 7,
  Unsupported = 8,
  Contradictory = 9,
};

// Worst-wins rank used when combining probe classifications into a zone
// classification, and zone classifications into an overall assessment. Higher
// rank is worse; the order is part of the contract and is tested.
[[nodiscard]] constexpr std::uint8_t freshness_rank(FreshnessClass value) noexcept {
  switch (value) {
    case FreshnessClass::Fresh:
      return 0;
    case FreshnessClass::Stale:
      return 1;
    case FreshnessClass::Future:
      return 2;
    case FreshnessClass::Absent:
      return 3;
    case FreshnessClass::Suspect:
      return 4;
    case FreshnessClass::Unavailable:
      return 5;
    case FreshnessClass::Indeterminate:
      return 6;
    case FreshnessClass::Unsupported:
      return 7;
    case FreshnessClass::Contradictory:
      return 8;
  }
  return 8;
}

[[nodiscard]] constexpr FreshnessClass freshness_worst(FreshnessClass a, FreshnessClass b) noexcept {
  return freshness_rank(a) >= freshness_rank(b) ? a : b;
}

[[nodiscard]] std::string_view freshness_class_name(FreshnessClass value) noexcept;
[[nodiscard]] constexpr bool freshness_is_fresh(FreshnessClass value) noexcept {
  return value == FreshnessClass::Fresh;
}
// True when the classification is a positive statement about the zone rather
// than missing evidence.
[[nodiscard]] constexpr bool freshness_has_evidence(FreshnessClass value) noexcept {
  return value == FreshnessClass::Fresh || value == FreshnessClass::Suspect ||
         value == FreshnessClass::Contradictory;
}

// Derived assessment of one probe (never persisted as authority).
struct ProbeAssessment {
  ThermalProbeRef probe{};
  FreshnessClass cls{FreshnessClass::Absent};
  bool has_temperature{false};
  MilliCelsius temperature{};
  bool has_rate{false};
  MilliCelsiusPerMinute rate{};
  SampleQuality quality{SampleQuality::Unavailable};
  SensorHealth health{SensorHealth::Unknown};
  SampleOrigin origin{SampleOrigin::Recovered};
  EvidenceGeneration generation{};
  Timestamp observed_at{};
  Duration age{};
};

// Derived assessment of one thermal zone.
struct ZoneAssessment {
  ThermalZoneRef zone{};
  FreshnessClass cls{FreshnessClass::Absent};
  bool has_temperature{false};
  MilliCelsius max_temperature{};
  bool has_rate{false};
  MilliCelsiusPerMinute peak_rate{};
  std::uint32_t fresh_probes{0};
  std::uint32_t total_probes{0};
  std::uint32_t live_probes{0};
};

// Derived assessment of the whole monitored facility.
struct EvidenceAssessment {
  std::vector<ZoneAssessment> zones{};
  FreshnessClass worst{FreshnessClass::Fresh};
  bool all_fresh{true};
  bool any_evidence{false};
  std::uint32_t zones_with_evidence{0};
  Timestamp evaluated_at{};
};

// Severity justified by current evidence alone, before latching. Pure function:
// the same assessment and policy always yield the same severity.
//
// Rules:
//   * only Fresh zones with a temperature justify a static level;
//   * a rate at or above rapid_rate with temperature at or above
//     rapid_rate_floor justifies at least Critical;
//   * a rate at or above extreme_rate with temperature at or above
//     extreme_rate_floor justifies at least Emergency;
//   * when escalate_on_evidence_loss is set and any monitored zone is not
//     Fresh, the result is at least Warning (lost evidence is never treated as
//     recovered evidence).
[[nodiscard]] Severity justified_severity(const EvidenceAssessment& assessment,
                                          const ThermalPolicy& policy);

}  // namespace summon::tem
