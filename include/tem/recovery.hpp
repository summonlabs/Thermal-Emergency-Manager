// Thermal Emergency Manager -- recovery gates, hysteresis, and eligibility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tem/ids.hpp"
#include "tem/policy.hpp"
#include "tem/severity.hpp"

namespace summon::tem {

// Recovery is not "the temperature went down". Every gate below is evaluated
// explicitly, at a stated revision, against current evidence, and each result
// distinguishes passed, failed, unknown, and not-applicable.
enum class RecoveryGate : std::uint8_t {
  EvidenceFreshness = 1,     // every monitored probe is Fresh: live, good, in window
  EvidenceCompleteness = 2,  // every monitored zone has at least one live probe
  EvidenceConsistency = 3,   // no contradictory zone, no failed sensor without redundancy
  ThermalMargin = 4,         // every zone is below the hysteresis threshold for the level
  RateMargin = 5,            // no zone is rising faster than the recovery rate ceiling
  MitigationResolution = 6,  // every required class is Verified; none open or failed
  VerificationCurrency = 7,  // verification evidence is itself current
  Dwell = 8,                 // the margin condition has held continuously long enough
  InterlockIntegrity = 9,    // no protected obligation violated or unknown
  AuthorityCurrent = 10,     // no plan fenced by an epoch rollover is outstanding
  ClosureDwell = 11,         // for closure: the recovered state has held long enough
};

inline constexpr std::uint8_t kRecoveryGateCount = 11;

[[nodiscard]] std::string_view recovery_gate_name(RecoveryGate gate) noexcept;
[[nodiscard]] std::optional<RecoveryGate> recovery_gate_from_value(std::uint8_t value) noexcept;

enum class GateResult : std::uint8_t {
  Passed = 1,
  Failed = 2,
  Unknown = 3,  // evidence required by the gate is missing or not current
  NotApplicable = 4,
};

[[nodiscard]] std::string_view gate_result_name(GateResult value) noexcept;
[[nodiscard]] std::optional<GateResult> gate_result_from_value(std::uint8_t value) noexcept;

struct GateEvaluation {
  RecoveryGate gate{RecoveryGate::EvidenceFreshness};
  GateResult result{GateResult::Unknown};
  std::string detail{};
};

// The full recovery decision. The field named permitted_severity is the highest
// severity that current evidence and hysteresis permit; it is never below the
// severity justified by current evidence.
struct RecoveryEligibility {
  bool eligible{false};   // every applicable gate passed
  bool complete{false};   // an evaluation was produced, never a guess
  Severity current_severity{Severity::Nominal};
  Severity permitted_severity{Severity::Nominal};
  Severity justified{Severity::Nominal};
  std::vector<GateEvaluation> gates{};
  Timestamp evaluated_at{};
  StateRevision evaluated_revision{};
  std::uint32_t passed{0};
  std::uint32_t failed{0};
  std::uint32_t unknown{0};
  std::uint32_t not_applicable{0};

  [[nodiscard]] const GateEvaluation* find(RecoveryGate gate) const noexcept;
  [[nodiscard]] GateResult result_of(RecoveryGate gate) const noexcept;
  [[nodiscard]] std::string summarize() const;
};

// Incident disposition recorded at closure.
enum class Disposition : std::uint8_t {
  Recovered = 1,       // recovery verified, mitigations released
  Mitigated = 2,       // excursion contained by verified mitigations
  OperatorClosed = 3,  // closed by operator authority with an explicit reason
};

[[nodiscard]] std::string_view disposition_name(Disposition value) noexcept;
[[nodiscard]] std::optional<Disposition> disposition_from_value(std::uint8_t value) noexcept;

}  // namespace summon::tem
