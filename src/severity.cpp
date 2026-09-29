// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/severity.hpp"

#include <array>

namespace summon::tem {

std::string_view severity_name(Severity severity) noexcept {
  switch (severity) {
    case Severity::Nominal: return "nominal";
    case Severity::Advisory: return "advisory";
    case Severity::Warning: return "warning";
    case Severity::Critical: return "critical";
    case Severity::Emergency: return "emergency";
    case Severity::Catastrophic: return "catastrophic";
  }
  return "unknown";
}

std::optional<Severity> severity_from_value(std::uint8_t value) noexcept {
  if (value >= kSeverityCount) {
    return std::nullopt;
  }
  return severity_from_ordinal(value);
}

std::string_view lifecycle_name(Lifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case Lifecycle::None: return "none";
    case Lifecycle::Active: return "active";
    case Lifecycle::Stabilizing: return "stabilizing";
    case Lifecycle::Recovering: return "recovering";
    case Lifecycle::Recovered: return "recovered";
    case Lifecycle::Closed: return "closed";
  }
  return "unknown";
}

std::optional<Lifecycle> lifecycle_from_value(std::uint8_t value) noexcept {
  if (value >= kLifecycleCount) {
    return std::nullopt;
  }
  return static_cast<Lifecycle>(value);
}

// The lifecycle graph. Closed is terminal; recovery progress is never skipped:
// an incident must pass through Stabilizing and Recovering to be Recovered.
bool lifecycle_transition_allowed(Lifecycle from, Lifecycle to) noexcept {
  if (from == to) {
    return true;
  }
  switch (from) {
    case Lifecycle::None:
      return to == Lifecycle::Active;
    case Lifecycle::Active:
      return to == Lifecycle::Stabilizing || to == Lifecycle::Closed;
    case Lifecycle::Stabilizing:
      return to == Lifecycle::Active || to == Lifecycle::Recovering || to == Lifecycle::Closed;
    case Lifecycle::Recovering:
      return to == Lifecycle::Active || to == Lifecycle::Recovered || to == Lifecycle::Closed;
    case Lifecycle::Recovered:
      return to == Lifecycle::Active || to == Lifecycle::Closed;
    case Lifecycle::Closed:
      return false;
  }
  return false;
}

std::array<Severity, kSeverityCount> severity_ladder() noexcept {
  return {Severity::Nominal, Severity::Advisory, Severity::Warning, Severity::Critical,
          Severity::Emergency, Severity::Catastrophic};
}

}  // namespace summon::tem
