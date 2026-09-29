// Thermal Emergency Manager -- severity ladder.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include "tem/ids.hpp"

namespace summon::tem {

// Severity is an ordered ladder, never a free-form "emergency" flag. The
// numeric value is part of the persisted format and of the public contract.
enum class Severity : std::uint8_t {
  Nominal = 0,
  Advisory = 1,
  Warning = 2,
  Critical = 3,
  Emergency = 4,
  Catastrophic = 5,
};

inline constexpr std::uint8_t kSeverityCount = 6;
inline constexpr Severity kMaxSeverity = Severity::Catastrophic;

[[nodiscard]] std::string_view severity_name(Severity severity) noexcept;
[[nodiscard]] std::optional<Severity> severity_from_value(std::uint8_t value) noexcept;
[[nodiscard]] constexpr std::uint8_t severity_ordinal(Severity severity) noexcept {
  return static_cast<std::uint8_t>(severity);
}
[[nodiscard]] constexpr Severity severity_from_ordinal(std::uint8_t ordinal) noexcept {
  return static_cast<Severity>(ordinal);
}
[[nodiscard]] constexpr Severity severity_max(Severity a, Severity b) noexcept {
  return severity_ordinal(a) >= severity_ordinal(b) ? a : b;
}
[[nodiscard]] constexpr Severity severity_min(Severity a, Severity b) noexcept {
  return severity_ordinal(a) <= severity_ordinal(b) ? a : b;
}

// Incident lifecycle. Severity is latched while the incident is Active or
// Stabilizing; it can only move through the documented recovery path.
enum class Lifecycle : std::uint8_t {
  None = 0,         // no incident has been opened
  Active = 1,       // escalation phase: severity may only rise
  Stabilizing = 2,  // recovery preconditions hold; an explicit recovery request is required
  Recovering = 3,   // recovery steps are being taken under hysteresis and dwell
  Recovered = 4,    // recovery conditions met at the current revision
  Closed = 5,       // terminal: a later excursion opens a new incident generation
};

inline constexpr std::uint8_t kLifecycleCount = 6;

[[nodiscard]] std::string_view lifecycle_name(Lifecycle lifecycle) noexcept;
[[nodiscard]] std::optional<Lifecycle> lifecycle_from_value(std::uint8_t value) noexcept;
[[nodiscard]] constexpr std::uint8_t lifecycle_ordinal(Lifecycle lifecycle) noexcept {
  return static_cast<std::uint8_t>(lifecycle);
}

// Lifecycle transition legality. Severity is not involved here: see
// severity.hpp for the severity rules. Illegal transitions are refused with
// IllegalTransition and never partially applied.
[[nodiscard]] bool lifecycle_transition_allowed(Lifecycle from, Lifecycle to) noexcept;

// Ordered list of the severity levels, lowest first, for reporting.
[[nodiscard]] std::array<Severity, kSeverityCount> severity_ladder() noexcept;

}  // namespace summon::tem
