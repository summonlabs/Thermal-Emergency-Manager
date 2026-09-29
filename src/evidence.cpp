// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/evidence.hpp"

namespace summon::tem {
namespace {

bool quality_carries_reading(SampleQuality quality) noexcept {
  return quality == SampleQuality::Good || quality == SampleQuality::Suspect;
}

}  // namespace

std::string_view sample_quality_name(SampleQuality value) noexcept {
  switch (value) {
    case SampleQuality::Good: return "good";
    case SampleQuality::Suspect: return "suspect";
    case SampleQuality::Unavailable: return "unavailable";
    case SampleQuality::Unsupported: return "unsupported";
    case SampleQuality::Indeterminate: return "indeterminate";
  }
  return "unknown";
}

std::optional<SampleQuality> sample_quality_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return SampleQuality::Good;
    case 2: return SampleQuality::Suspect;
    case 3: return SampleQuality::Unavailable;
    case 4: return SampleQuality::Unsupported;
    case 5: return SampleQuality::Indeterminate;
    default: return std::nullopt;
  }
}

std::string_view sensor_health_name(SensorHealth value) noexcept {
  switch (value) {
    case SensorHealth::Healthy: return "healthy";
    case SensorHealth::Degraded: return "degraded";
    case SensorHealth::Failed: return "failed";
    case SensorHealth::Unknown: return "unknown";
  }
  return "unknown";
}

std::optional<SensorHealth> sensor_health_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return SensorHealth::Healthy;
    case 2: return SensorHealth::Degraded;
    case 3: return SensorHealth::Failed;
    case 4: return SensorHealth::Unknown;
    default: return std::nullopt;
  }
}

std::string_view sample_origin_name(SampleOrigin value) noexcept {
  switch (value) {
    case SampleOrigin::Live: return "live";
    case SampleOrigin::Recovered: return "recovered";
  }
  return "unknown";
}

std::optional<SampleOrigin> sample_origin_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return SampleOrigin::Live;
    case 2: return SampleOrigin::Recovered;
    default: return std::nullopt;
  }
}

std::string_view freshness_class_name(FreshnessClass value) noexcept {
  switch (value) {
    case FreshnessClass::Fresh: return "fresh";
    case FreshnessClass::Stale: return "stale";
    case FreshnessClass::Future: return "future";
    case FreshnessClass::Absent: return "absent";
    case FreshnessClass::Suspect: return "suspect";
    case FreshnessClass::Unavailable: return "unavailable";
    case FreshnessClass::Indeterminate: return "indeterminate";
    case FreshnessClass::Unsupported: return "unsupported";
    case FreshnessClass::Contradictory: return "contradictory";
  }
  return "unknown";
}

Status validate_sample_shape(const ThermalSample& sample, const ThermalPolicy& policy) {
  if (!sample.zone.valid()) {
    return Status::error(StatusCode::EmptyField, "sample has no thermal zone reference");
  }
  if (!sample.probe.valid()) {
    return Status::error(StatusCode::EmptyField, "sample has no probe reference");
  }
  if (!sample.source.valid()) {
    return Status::error(StatusCode::EmptyField, "sample has no source reference");
  }
  if (sample.sequence.is_absent()) {
    return Status::error(StatusCode::ValueOutOfRange, "sample sequence must be non-zero")
        .with_context(sample.probe.text());
  }
  const bool carries = quality_carries_reading(sample.quality);
  if (carries && !sample.has_temperature) {
    return Status::error(StatusCode::InvalidArgument,
                         "a good or suspect sample must carry a temperature reading")
        .with_context(sample.probe.text());
  }
  if (!carries && sample.has_temperature) {
    return Status::error(StatusCode::InvalidArgument,
                         "a sample that carries no reading must not carry a temperature")
        .with_context(sample.probe.text());
  }
  if (sample.has_rate && !carries) {
    return Status::error(StatusCode::InvalidArgument,
                         "a sample that carries no reading must not carry a rate")
        .with_context(sample.probe.text());
  }
  const auto limit = sample.received_at.checked_add(policy.evidence_future_skew);
  if (!limit.has_value()) {
    return Status::error(StatusCode::ValueOutOfRange, "sample timestamp overflow")
        .with_context(sample.probe.text());
  }
  if (sample.observed_at > *limit) {
    return Status::error(StatusCode::EvidenceFuture,
                         "sample was observed beyond the permitted clock skew")
        .with_context(sample.probe.text());
  }
  return Status::success();
}

Severity justified_severity(const EvidenceAssessment& assessment, const ThermalPolicy& policy) {
  Severity result = Severity::Nominal;
  for (const ZoneAssessment& zone : assessment.zones) {
    // A zone with at least one current reading drives escalation even when
    // another probe in the same zone is stale or absent: missing evidence must
    // never hide an excursion that is still being measured. The evidence-loss
    // floor below raises the result further when anything is not current.
    if (!zone.has_temperature) {
      continue;
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
    result = severity_max(result, level);
  }
  if (policy.escalate_on_evidence_loss && !assessment.all_fresh) {
    result = severity_max(result, Severity::Warning);
  }
  return result;
}

}  // namespace summon::tem
