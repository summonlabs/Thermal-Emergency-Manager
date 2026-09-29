// Thermal Emergency Manager -- public emergency runtime API.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "tem/authority.hpp"
#include "tem/evidence.hpp"
#include "tem/journal.hpp"
#include "tem/mitigation.hpp"
#include "tem/obligations.hpp"
#include "tem/plan.hpp"
#include "tem/recovery.hpp"
#include "tem/state.hpp"
#include "tem/store.hpp"
#include "tem/transport.hpp"

namespace summon::tem {

struct RuntimeOptions {
  std::filesystem::path store_root{};
  ControllerIncarnation incarnation{};
  ControlEpoch epoch{};
  ThermalPolicy policy{};
  RelaxationPolicy relaxation{};
  DurabilityMode durability{DurabilityMode::Durable};
  bool create_if_missing{true};
  // A store that was written under a different policy generation is refused
  // unless the caller explicitly adopts the new policy, which is recorded as an
  // authoritative journal entry.
  bool adopt_policy_changes{false};
  IMitigationTransport* transport{nullptr};
};

struct RuntimeStats {
  std::uint64_t commits{0};
  std::uint64_t ticks{0};
  std::uint64_t ticks_changed{0};
  std::uint64_t requests_issued{0};
  std::uint64_t requests_dispatched{0};
  std::uint64_t dispatch_indeterminate{0};
  std::uint64_t dispatch_failures{0};
  std::uint64_t deduped_retries{0};
  std::uint64_t evidence_admitted{0};
  std::uint64_t evidence_rejected{0};
  std::uint64_t escalations{0};
  std::uint64_t deescalations{0};
  std::uint64_t relaxations{0};
  std::uint64_t replay_checks{0};
  std::uint64_t replay_divergences{0};
  std::uint64_t journal_dropped{0};
  std::uint64_t obligations_registered{0};
  std::uint64_t store_recoveries{0};
};

struct TickReport {
  bool state_changed{false};
  Severity severity_before{Severity::Nominal};
  Severity severity_after{Severity::Nominal};
  Lifecycle lifecycle_before{Lifecycle::None};
  Lifecycle lifecycle_after{Lifecycle::None};
  std::uint32_t requests_issued{0};
  std::uint32_t requests_dispatched{0};
  std::uint32_t requests_indeterminate{0};
  std::uint32_t requests_failed{0};
  std::uint32_t requests_expired{0};
  std::uint32_t requests_abandoned{0};
  std::uint32_t requests_superseded{0};
  StateRevision revision{};
  Timestamp evaluated_at{};
  RecoveryEligibility eligibility{};
};

struct MitigationRequestDraft {
  MitigationClass cls{MitigationClass::DerateAccelerators};
  RequestTarget target{};
  BasisPoints intensity{};
  RequestReason reason{RequestReason::OperatorRequest};
  std::vector<RefToken> evidence_refs{};
};

struct IssueOptions {
  bool has_client_key{false};
  IdempotencyKey client_key{};
  bool has_expiry{true};
  Timestamp expires_at{};
  bool has_verification_deadline{true};
  Timestamp verification_deadline{};
};

struct ReplayReport {
  bool verified{false};
  std::uint32_t journal_entries{0};
  StateRevision checkpoint_revision{};
  StateRevision reconstructed_revision{};
  StateRevision live_revision{};
  std::uint32_t incidents{0};
  std::uint32_t requests{0};
  std::string detail{};
};

// A frozen copy of everything an inspector needs. Callers never receive
// references into live mutable state.
struct ReadModel {
  ControllerIncarnation incarnation{};
  ControlEpoch epoch{};
  StateRevision revision{};
  CommitSequence commit_sequence{};
  StoreGeneration store_generation{};
  bool recovered_from_store{false};
  DurabilityMode durability{DurabilityMode::Durable};
  std::string store_root{};

  bool incident_open{false};
  IncidentId incident{};
  IncidentGeneration generation{};
  Severity severity{Severity::Nominal};
  Severity peak_severity{Severity::Nominal};
  Lifecycle lifecycle{Lifecycle::None};
  Timestamp opened_at{};
  Timestamp closed_at{};
  bool has_disposition{false};
  Disposition disposition{Disposition::Recovered};
  std::uint64_t fenced_requests{0};

  ThermalPolicy policy{};
  RelaxationPolicy relaxation{};
  EvidenceAssessment evidence{};
  std::vector<ZoneSlot> zones{};
  std::vector<ProtectedObligation> obligations{};
  std::vector<MitigationRecord> requests{};
  std::vector<RelaxationRecord> relaxations{};
  std::vector<TransitionRecord> transitions{};
  std::uint64_t transitions_dropped{0};
  RecoveryEligibility eligibility{};
  RuntimeStats stats{};
};

// Builds an inspection model directly from durable state, without opening a
// runtime. Used by read-only inspection so that looking at a store never
// mutates it.
[[nodiscard]] ReadModel build_read_model(const DomainState& state, const StoreSnapshot& snapshot,
                                         Timestamp now, const RuntimeStats& stats);

// The thermal emergency runtime.
//
// Concurrency model: one writer. Durable mutation is serialised by an OS-level
// exclusive lock on the store directory, and in-process state is guarded by a
// single mutex. Adapter code (the mitigation transport) is never called while
// that mutex is held: a tick computes and persists its decisions, releases the
// lock, dispatches, and then records the dispatch outcomes in a second
// committed step. That ordering is what makes the persisted intent the
// write-ahead record for every external request.
class EmergencyRuntime {
 public:
  [[nodiscard]] static Result<std::unique_ptr<EmergencyRuntime>> Open(const RuntimeOptions& options);
  ~EmergencyRuntime();
  EmergencyRuntime(const EmergencyRuntime&) = delete;
  EmergencyRuntime& operator=(const EmergencyRuntime&) = delete;

  // The token a caller must present for the next mutation. It carries the live
  // revision, so any observation the caller made is bound to a revision.
  [[nodiscard]] AuthorityToken Authority() const;
  [[nodiscard]] bool closed() const noexcept;

  // --- facility configuration -------------------------------------------
  [[nodiscard]] VoidResult RegisterZone(const AuthorityToken& token, ThermalZoneRef zone);
  [[nodiscard]] VoidResult RegisterProbe(const AuthorityToken& token, ThermalZoneRef zone,
                                         ThermalProbeRef probe);
  [[nodiscard]] VoidResult RegisterTargetBinding(const AuthorityToken& token,
                                                 const TargetBinding& binding);
  [[nodiscard]] Result<ObligationId> RegisterObligation(const AuthorityToken& token,
                                                        ProtectedObligation obligation);
  [[nodiscard]] VoidResult ReportObligationStatus(const AuthorityToken& token, ObligationId id,
                                                  ObligationStatus status,
                                                  ExternalAuthorityRef source, Timestamp status_at);

  // --- emergency evidence -------------------------------------------------
  [[nodiscard]] VoidResult AdmitSample(const AuthorityToken& token, const ThermalSample& sample);

  // --- decisions ----------------------------------------------------------
  [[nodiscard]] Result<TickReport> Tick(const AuthorityToken& token, Timestamp now);
  [[nodiscard]] RecoveryEligibility EvaluateRecovery(Timestamp now, bool for_closure) const;
  [[nodiscard]] Result<EscalationPlan> Plan(Timestamp now) const;
  [[nodiscard]] VoidResult BeginRecovery(const AuthorityToken& token, OperatorRef operator_ref,
                                         Timestamp now);
  [[nodiscard]] VoidResult CloseIncident(const AuthorityToken& token, OperatorRef operator_ref,
                                         Disposition disposition, Timestamp now);

  // --- mitigation request lifecycle --------------------------------------
  [[nodiscard]] Result<MitigationRequest> IssueRequest(const AuthorityToken& token,
                                                       const MitigationRequestDraft& draft,
                                                       const IssueOptions& options, Timestamp now);
  [[nodiscard]] VoidResult AcknowledgeRequest(const AuthorityToken& token, MitigationRequestId id,
                                              ExternalEvidenceRef external_ref, Timestamp at);
  [[nodiscard]] VoidResult ObserveRequest(const AuthorityToken& token, MitigationRequestId id,
                                          ExternalEvidenceRef external_ref,
                                          ObservationSequence observation, Timestamp at);
  [[nodiscard]] VoidResult VerifyRequest(const AuthorityToken& token, MitigationRequestId id,
                                         ExternalEvidenceRef verification_ref, Timestamp at);
  [[nodiscard]] VoidResult FailRequest(const AuthorityToken& token, MitigationRequestId id,
                                       FailureCode failure, std::int32_t transport_code,
                                       Timestamp at);
  [[nodiscard]] VoidResult AbandonRequest(const AuthorityToken& token, MitigationRequestId id,
                                          Timestamp at);

  // --- protected obligations ---------------------------------------------
  [[nodiscard]] VoidResult RelaxConstraint(const AuthorityToken& token, ObligationId id,
                                           MitigationClass cls, ExternalAuthorityRef authority,
                                           Timestamp at);
  [[nodiscard]] VoidResult ReimposeConstraint(const AuthorityToken& token, ObligationId id,
                                              Timestamp at);

  // --- inspection ---------------------------------------------------------
  [[nodiscard]] ReadModel Inspect(Timestamp now) const;
  [[nodiscard]] Result<ReplayReport> VerifyReplay() const;
  [[nodiscard]] StoreVerification VerifyStore() const;
  [[nodiscard]] RuntimeStats Stats() const;
  [[nodiscard]] const std::filesystem::path& store_root() const noexcept;

  // Stops accepting new work and releases the store lock. Idempotent.
  void Shutdown();

 private:
  EmergencyRuntime() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

}  // namespace summon::tem
