// Thermal Emergency Manager -- text rendering for inspection and audit output.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "tem/journal.hpp"
#include "tem/plan.hpp"
#include "tem/recovery.hpp"
#include "tem/runtime.hpp"
#include "tem/store.hpp"

namespace summon::tem::report {

[[nodiscard]] std::string render_stats(const RuntimeStats& stats);
[[nodiscard]] std::string render_eligibility(const RecoveryEligibility& eligibility);
[[nodiscard]] std::string render_plan(const EscalationPlan& plan);
[[nodiscard]] std::string render_requests(const std::vector<MitigationRecord>& requests);
[[nodiscard]] std::string render_obligations(const std::vector<ProtectedObligation>& obligations);
[[nodiscard]] std::string render_transitions(const std::vector<TransitionRecord>& transitions,
                                             std::size_t max_lines);
[[nodiscard]] std::string render_evidence(const EvidenceAssessment& assessment);
[[nodiscard]] std::string render_journal(const std::vector<JournalEntry>& journal, std::size_t max_lines);
[[nodiscard]] std::string render_journal_entry(const JournalEntry& entry);
[[nodiscard]] std::string render_store_verification(const StoreVerification& verification);
[[nodiscard]] std::string render_replay(const ReplayReport& replay);
[[nodiscard]] std::string render_read_model(const ReadModel& model);
[[nodiscard]] std::string render_status(const Status& status);

}  // namespace summon::tem::report
