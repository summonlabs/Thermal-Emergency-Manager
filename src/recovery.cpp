// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/recovery.hpp"

#include <string>

namespace summon::tem {

std::string_view recovery_gate_name(RecoveryGate gate) noexcept {
  switch (gate) {
    case RecoveryGate::EvidenceFreshness: return "evidence-freshness";
    case RecoveryGate::EvidenceCompleteness: return "evidence-completeness";
    case RecoveryGate::EvidenceConsistency: return "evidence-consistency";
    case RecoveryGate::ThermalMargin: return "thermal-margin";
    case RecoveryGate::RateMargin: return "rate-margin";
    case RecoveryGate::MitigationResolution: return "mitigation-resolution";
    case RecoveryGate::VerificationCurrency: return "verification-currency";
    case RecoveryGate::Dwell: return "dwell";
    case RecoveryGate::InterlockIntegrity: return "interlock-integrity";
    case RecoveryGate::AuthorityCurrent: return "authority-current";
    case RecoveryGate::ClosureDwell: return "closure-dwell";
  }
  return "unknown";
}

std::optional<RecoveryGate> recovery_gate_from_value(std::uint8_t value) noexcept {
  if (value < 1 || value > kRecoveryGateCount) {
    return std::nullopt;
  }
  return static_cast<RecoveryGate>(value);
}

std::string_view gate_result_name(GateResult value) noexcept {
  switch (value) {
    case GateResult::Passed: return "passed";
    case GateResult::Failed: return "failed";
    case GateResult::Unknown: return "unknown";
    case GateResult::NotApplicable: return "not-applicable";
  }
  return "unknown";
}

std::optional<GateResult> gate_result_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return GateResult::Passed;
    case 2: return GateResult::Failed;
    case 3: return GateResult::Unknown;
    case 4: return GateResult::NotApplicable;
    default: return std::nullopt;
  }
}

std::string_view disposition_name(Disposition value) noexcept {
  switch (value) {
    case Disposition::Recovered: return "recovered";
    case Disposition::Mitigated: return "mitigated";
    case Disposition::OperatorClosed: return "operator-closed";
  }
  return "unknown";
}

std::optional<Disposition> disposition_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return Disposition::Recovered;
    case 2: return Disposition::Mitigated;
    case 3: return Disposition::OperatorClosed;
    default: return std::nullopt;
  }
}

const GateEvaluation* RecoveryEligibility::find(RecoveryGate gate) const noexcept {
  for (const GateEvaluation& evaluation : gates) {
    if (evaluation.gate == gate) {
      return &evaluation;
    }
  }
  return nullptr;
}

GateResult RecoveryEligibility::result_of(RecoveryGate gate) const noexcept {
  const GateEvaluation* evaluation = find(gate);
  return evaluation != nullptr ? evaluation->result : GateResult::Unknown;
}

std::string RecoveryEligibility::summarize() const {
  std::string out;
  out.append(eligible ? "eligible" : "not-eligible");
  out.append(" severity=");
  out.append(severity_name(current_severity));
  out.append(" permitted=");
  out.append(severity_name(permitted_severity));
  out.append(" justified=");
  out.append(severity_name(justified));
  out.append(" gates[passed=");
  out.append(std::to_string(passed));
  out.append(" failed=");
  out.append(std::to_string(failed));
  out.append(" unknown=");
  out.append(std::to_string(unknown));
  out.append(" n/a=");
  out.append(std::to_string(not_applicable));
  out.push_back(']');
  for (const GateEvaluation& evaluation : gates) {
    if (evaluation.result == GateResult::Failed || evaluation.result == GateResult::Unknown) {
      out.push_back(' ');
      out.append(recovery_gate_name(evaluation.gate));
      out.push_back('=');
      out.append(gate_result_name(evaluation.result));
      if (!evaluation.detail.empty()) {
        out.push_back('(');
        out.append(evaluation.detail);
        out.push_back(')');
      }
    }
  }
  return out;
}

}  // namespace summon::tem
