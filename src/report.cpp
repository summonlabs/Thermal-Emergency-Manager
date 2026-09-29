// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/report.hpp"

#include <algorithm>
#include <string>

#include "tem/plan.hpp"

namespace summon::tem::report {
namespace {

std::string pad(std::string_view text, std::size_t width) {
  std::string out(text);
  while (out.size() < width) {
    out.push_back(' ');
  }
  return out;
}

std::string line(const std::string& text) { return text + "\n"; }

}  // namespace

std::string render_status(const Status& status) {
  return status.to_string();
}

std::string render_stats(const RuntimeStats& stats) {
  std::string out;
  out += line("commits=" + std::to_string(stats.commits) + " ticks=" +
              std::to_string(stats.ticks) + " changed_ticks=" +
              std::to_string(stats.ticks_changed));
  out += line("requests issued=" + std::to_string(stats.requests_issued) +
              " dispatched=" + std::to_string(stats.requests_dispatched) +
              " indeterminate=" + std::to_string(stats.dispatch_indeterminate) +
              " dispatch_failures=" + std::to_string(stats.dispatch_failures));
  out += line("deduped_retries=" + std::to_string(stats.deduped_retries) +
              " evidence_admitted=" + std::to_string(stats.evidence_admitted) +
              " evidence_rejected=" + std::to_string(stats.evidence_rejected));
  out += line("escalations=" + std::to_string(stats.escalations) +
              " deescalations=" + std::to_string(stats.deescalations) +
              " relaxations=" + std::to_string(stats.relaxations) +
              " store_recoveries=" + std::to_string(stats.store_recoveries));
  out += line("replay_checks=" + std::to_string(stats.replay_checks) +
              " replay_divergences=" + std::to_string(stats.replay_divergences) +
              " journal_dropped=" + std::to_string(stats.journal_dropped));
  return out;
}

std::string render_eligibility(const RecoveryEligibility& eligibility) {
  std::string out;
  out += line(std::string("recovery: ") + (eligibility.eligible ? "ELIGIBLE" : "NOT ELIGIBLE") +
              " severity=" + std::string(severity_name(eligibility.current_severity)) +
              " permitted=" + std::string(severity_name(eligibility.permitted_severity)) +
              " justified=" + std::string(severity_name(eligibility.justified)) +
              " revision=" + std::to_string(eligibility.evaluated_revision.value()));
  for (const GateEvaluation& evaluation : eligibility.gates) {
    std::string text = "  " + pad(recovery_gate_name(evaluation.gate), 24) +
                       std::string(gate_result_name(evaluation.result));
    if (!evaluation.detail.empty()) {
      text += "  " + evaluation.detail;
    }
    out += line(text);
  }
  return out;
}

std::string render_plan(const EscalationPlan& plan) {
  std::string out;
  out += line(std::string("plan: severity=") + std::string(severity_name(plan.severity)) +
              " justified=" + std::string(severity_name(plan.justified)) +
              " lifecycle=" + std::string(lifecycle_name(plan.lifecycle)) +
              " evidence_lost=" + (plan.evidence_lost ? "yes" : "no"));
  for (const PlannedRequest& request : plan.requests) {
    out += line("  request " + pad(std::string(mitigation_class_name(request.cls)), 22) +
                request.target.to_string() + " intensity=" + format_basis_points(request.intensity));
  }
  for (const MitigationRequestId id : plan.abandon) {
    out += line("  abandon request " + std::to_string(id.value()));
  }
  if (plan.requests.empty() && plan.abandon.empty()) {
    out += line("  no action required");
  }
  return out;
}

std::string render_requests(const std::vector<MitigationRecord>& requests) {
  std::string out;
  std::vector<const MitigationRecord*> ordered;
  ordered.reserve(requests.size());
  for (const MitigationRecord& record : requests) {
    ordered.push_back(&record);
  }
  std::sort(ordered.begin(), ordered.end(), [](const MitigationRecord* a, const MitigationRecord* b) {
    return order_key_of(a->request) < order_key_of(b->request);
  });
  for (const MitigationRecord* record : ordered) {
    std::string text = "  #" + std::to_string(record->request.id.value()) +
                       " attempt=" + std::to_string(record->request.attempt.value()) + " " +
                       pad(std::string(mitigation_class_name(record->request.cls)), 22) + " " +
                       pad(record->request.target.to_string(), 32) + " " +
                       pad(std::string(mitigation_state_name(record->state)), 13) +
                       " key=" + record->request.key.to_hex().substr(0, 12);
    if (record->state == MitigationState::Failed) {
      text += " failure=" + std::string(failure_code_name(record->failure));
    }
    if (record->state == MitigationState::Superseded) {
      text += " reason=" + std::string(supersede_reason_name(record->supersede));
    }
    if (record->has_external_ref) {
      text += " ref=" + record->external_ref.text();
    }
    out += line(text);
  }
  if (ordered.empty()) {
    out += line("  none");
  }
  return out;
}

std::string render_obligations(const std::vector<ProtectedObligation>& obligations) {
  std::string out;
  for (const ProtectedObligation& obligation : obligations) {
    std::string text = "  #" + std::to_string(obligation.id.value()) + " " +
                       pad(obligation.ref, 28) + " " +
                       pad(std::string(protection_class_name(obligation.protection)), 22) +
                       " forbids=";
    bool first = true;
    for (std::uint8_t i = 1; i <= kMitigationClassCount; ++i) {
      const auto cls = mitigation_class_from_value(i);
      if (!cls.has_value() || !obligation_forbids(obligation, *cls)) {
        continue;
      }
      if (!first) {
        text += ",";
      }
      text += std::string(mitigation_class_name(*cls));
      first = false;
    }
    text += " status=" + std::string(obligation_status_name(obligation.status));
    if (obligation.status_present) {
      text += " at=" + format_timestamp(obligation.status_at);
    }
    out += line(text);
  }
  if (obligations.empty()) {
    out += line("  none");
  }
  return out;
}

std::string render_transitions(const std::vector<TransitionRecord>& transitions, std::size_t max_lines) {
  std::string out;
  const std::size_t start = transitions.size() > max_lines ? transitions.size() - max_lines : 0;
  for (std::size_t i = start; i < transitions.size(); ++i) {
    const TransitionRecord& record = transitions[i];
    std::string text = "  " + format_timestamp(record.at) + " #" +
                       std::to_string(record.sequence.value()) +
                       " sev " + std::string(severity_name(record.from_severity)) + " -> " +
                       std::string(severity_name(record.to_severity)) + " life " +
                       std::string(lifecycle_name(record.from_lifecycle)) + " -> " +
                       std::string(lifecycle_name(record.to_lifecycle)) + " (" +
                       std::string(transition_trigger_name(record.trigger)) + ") " + record.detail;
    out += line(text);
  }
  if (transitions.empty()) {
    out += line("  none");
  }
  return out;
}

std::string render_evidence(const EvidenceAssessment& assessment) {
  std::string out;
  out += line(std::string("evidence at ") + format_timestamp(assessment.evaluated_at) +
              ": worst=" + std::string(freshness_class_name(assessment.worst)) +
              " all_fresh=" + (assessment.all_fresh ? "yes" : "no"));
  for (const ZoneAssessment& zone : assessment.zones) {
    std::string text = "  " + pad(zone.zone.text(), 24) + " " +
                       pad(std::string(freshness_class_name(zone.cls)), 14) + " fresh=" +
                       std::to_string(zone.fresh_probes) + "/" + std::to_string(zone.total_probes);
    if (zone.has_temperature) {
      text += " max=" + format_temperature(zone.max_temperature);
    }
    if (zone.has_rate) {
      text += " peak=" + format_rate(zone.peak_rate);
    }
    out += line(text);
  }
  return out;
}

std::string render_journal_entry(const JournalEntry& entry) {
  std::string text = format_timestamp(entry.at) + " #" + std::to_string(entry.sequence.value()) +
                     " rev " + std::to_string(entry.revision_before.value()) + "->" +
                     std::to_string(entry.revision_after.value()) + " epoch=" +
                     std::to_string(entry.epoch.value()) + " " +
                     pad(std::string(journal_kind_name(entry.kind)), 24);
  std::visit(
      [&text](const auto& payload) {
        using PayloadType = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<PayloadType, IncidentOpenedPayload>) {
          text += payload.trigger_zone.text() + " initial=" + std::string(severity_name(payload.initial));
        } else if constexpr (std::is_same_v<PayloadType, ZoneRegisteredPayload>) {
          text += payload.zone.text();
        } else if constexpr (std::is_same_v<PayloadType, ProbeRegisteredPayload>) {
          text += payload.zone.text() + "/" + payload.probe.text();
        } else if constexpr (std::is_same_v<PayloadType, ObligationRegisteredPayload>) {
          text += payload.obligation.ref;
        } else if constexpr (std::is_same_v<PayloadType, EvidenceAdmittedPayload>) {
          text += payload.sample.probe.text() + " " +
                  std::string(sample_quality_name(payload.sample.quality));
          if (payload.sample.has_temperature) {
            text += " " + format_temperature(payload.sample.temperature);
          }
          text += " seq=" + std::to_string(payload.sample.sequence.value());
        } else if constexpr (std::is_same_v<PayloadType, ObligationStatusPayload>) {
          text += std::to_string(payload.obligation.value()) + " " +
                  std::string(obligation_status_name(payload.status));
        } else if constexpr (std::is_same_v<PayloadType, TickPayload>) {
          text += "zones=" + std::to_string(payload.evaluated_zones);
        } else if constexpr (std::is_same_v<PayloadType, RequestsIssuedPayload>) {
          for (const MitigationRequest& request : payload.requests) {
            text += "#" + std::to_string(request.id.value()) + " " +
                    std::string(mitigation_class_name(request.cls)) + " " +
                    request.target.to_string() + " ";
          }
        } else if constexpr (std::is_same_v<PayloadType, RequestAcknowledgedPayload>) {
          text += "#" + std::to_string(payload.request.value()) + " ref=" + payload.external_ref.text();
        } else if constexpr (std::is_same_v<PayloadType, RequestObservedPayload>) {
          text += "#" + std::to_string(payload.request.value()) + " ref=" + payload.external_ref.text();
        } else if constexpr (std::is_same_v<PayloadType, RequestVerifiedPayload>) {
          text += "#" + std::to_string(payload.request.value()) +
                  " ref=" + payload.verification_ref.text();
        } else if constexpr (std::is_same_v<PayloadType, RequestFailedPayload>) {
          text += "#" + std::to_string(payload.request.value()) + " " +
                  std::string(failure_code_name(payload.failure)) + " code=" +
                  std::to_string(payload.transport_code);
        } else if constexpr (std::is_same_v<PayloadType, RequestSupersededPayload>) {
          text += "#" + std::to_string(payload.request.value()) + " " +
                  std::string(supersede_reason_name(payload.reason));
        } else if constexpr (std::is_same_v<PayloadType, RequestAbandonedPayload>) {
          text += "#" + std::to_string(payload.request.value());
        } else if constexpr (std::is_same_v<PayloadType, ConstraintRelaxedPayload>) {
          text += std::to_string(payload.obligation.value()) + " " +
                  std::string(mitigation_class_name(payload.cls)) + " by " + payload.authority.text();
        } else if constexpr (std::is_same_v<PayloadType, ConstraintReimposedPayload>) {
          text += std::to_string(payload.obligation.value());
        } else if constexpr (std::is_same_v<PayloadType, RecoveryBegunPayload>) {
          text += payload.operator_ref.text();
        } else if constexpr (std::is_same_v<PayloadType, IncidentClosedPayload>) {
          text += payload.operator_ref.text() + " " + std::string(disposition_name(payload.disposition));
        } else if constexpr (std::is_same_v<PayloadType, AuthorityRolledOverPayload>) {
          text += "incarnation=" + std::to_string(payload.incarnation.value()) +
                  " epoch=" + std::to_string(payload.epoch.value());
        } else if constexpr (std::is_same_v<PayloadType, PolicyAdoptedPayload>) {
          text += "generation=" + std::to_string(payload.policy.generation.value());
        } else if constexpr (std::is_same_v<PayloadType, TargetBindingRegisteredPayload>) {
          text += payload.zone.text() + " domain=" + payload.power_domain.text();
        } else if constexpr (std::is_same_v<PayloadType, StoreRecoveredPayload>) {
          text += std::string("rollover=") + (payload.rollover ? "yes" : "no") + " previous_epoch=" +
                  std::to_string(payload.previous_epoch.value());
        }
      },
      entry.payload);
  return text;
}

std::string render_journal(const std::vector<JournalEntry>& journal, std::size_t max_lines) {
  std::string out;
  const std::size_t start = journal.size() > max_lines ? journal.size() - max_lines : 0;
  for (std::size_t i = start; i < journal.size(); ++i) {
    out += line(render_journal_entry(journal[i]));
  }
  if (journal.empty()) {
    out += line("  empty");
  }
  return out;
}

std::string render_store_verification(const StoreVerification& verification) {
  std::string out;
  out += line(std::string("store verification: ") + (verification.ok ? "OK" : "FAILED"));
  out += line("  slot 0: present=" + std::string(verification.slot_a_present ? "yes" : "no") +
              " valid=" + std::string(verification.slot_a_valid ? "yes" : "no") +
              " commit=" + std::to_string(verification.slot_a_commit.value()) +
              " bytes=" + std::to_string(verification.slot_a_bytes));
  out += line("  slot 1: present=" + std::string(verification.slot_b_present ? "yes" : "no") +
              " valid=" + std::string(verification.slot_b_valid ? "yes" : "no") +
              " commit=" + std::to_string(verification.slot_b_commit.value()) +
              " bytes=" + std::to_string(verification.slot_b_bytes));
  out += line("  authoritative slot " + std::to_string(verification.authoritative_slot) +
              (verification.authoritative_is_older ? " (older than the other slot)" : ""));
  if (!verification.detail.empty()) {
    out += line("  " + verification.detail);
  }
  return out;
}

std::string render_replay(const ReplayReport& replay) {
  std::string out;
  out += line(std::string("replay: ") + (replay.verified ? "VERIFIED" : "DIVERGED"));
  out += line("  entries=" + std::to_string(replay.journal_entries) +
              " checkpoint_revision=" + std::to_string(replay.checkpoint_revision.value()) +
              " reconstructed_revision=" + std::to_string(replay.reconstructed_revision.value()) +
              " live_revision=" + std::to_string(replay.live_revision.value()));
  out += line("  incidents=" + std::to_string(replay.incidents) +
              " requests=" + std::to_string(replay.requests));
  if (!replay.detail.empty()) {
    out += line("  " + replay.detail);
  }
  return out;
}

std::string render_read_model(const ReadModel& model) {
  std::string out;
  out += line("store: " + (model.store_root.empty() ? std::string("<volatile>") : model.store_root) +
              " durability=" +
              (model.durability == DurabilityMode::Durable ? "durable" : "memory-only"));
  out += line("authority: epoch=" + std::to_string(model.epoch.value()) +
              " incarnation=" + std::to_string(model.incarnation.value()) +
              " revision=" + std::to_string(model.revision.value()) +
              " commit=" + std::to_string(model.commit_sequence.value()) +
              " generation=" + std::to_string(model.store_generation.value()));
  if (model.incident_open || model.lifecycle != Lifecycle::None) {
    out += line("incident: id=" + std::to_string(model.incident.value()) +
                " generation=" + std::to_string(model.generation.value()) +
                " severity=" + std::string(severity_name(model.severity)) +
                " peak=" + std::string(severity_name(model.peak_severity)) +
                " lifecycle=" + std::string(lifecycle_name(model.lifecycle)) +
                " opened=" + format_timestamp(model.opened_at));
  } else {
    out += line("incident: none");
  }
  if (model.recovered_from_store) {
    out += line("state was restored from durable storage; restored readings are not current "
                "evidence until a live sample replaces them");
  }
  out += render_evidence(model.evidence);
  out += line("requests:");
  out += render_requests(model.requests);
  out += line("obligations:");
  out += render_obligations(model.obligations);
  out += render_eligibility(model.eligibility);
  out += line("recent transitions:");
  out += render_transitions(model.transitions, 12);
  out += render_stats(model.stats);
  return out;
}

}  // namespace summon::tem::report
