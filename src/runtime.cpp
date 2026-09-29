// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/runtime.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "tem/codec.hpp"
#include "tem/plan.hpp"

namespace summon::tem {
namespace {

// Dispatch re-entrancy guard. A transport that calls back into the runtime
// would deadlock or corrupt state; the call is refused instead.
thread_local int g_dispatch_depth = 0;

class DispatchScope {
 public:
  DispatchScope() { ++g_dispatch_depth; }
  ~DispatchScope() { --g_dispatch_depth; }
  DispatchScope(const DispatchScope&) = delete;
  DispatchScope& operator=(const DispatchScope&) = delete;
};

bool same_policy(const ThermalPolicy& a, const RelaxationPolicy& ra, const ThermalPolicy& b,
                 const RelaxationPolicy& rb) {
  codec::Writer left;
  codec::encode(left, a);
  codec::encode(left, ra);
  codec::Writer right;
  codec::encode(right, b);
  codec::encode(right, rb);
  return left.bytes() == right.bytes();
}

Status degraded_status(const Status& cause) {
  Status status = Status::error(StatusCode::StoreIoError,
                                "the runtime is fenced after a failed durable commit");
  status.with_context(cause.to_string());
  return status;
}

struct DispatchOutcomeRecord {
  MitigationRequest request;
  DispatchResult result;
};

const MitigationRecord* find_request_in(const DomainState& state, IncidentId incident,
                                        MitigationRequestId id) {
  const IncidentProjection* projection = find_incident(state, incident);
  return projection == nullptr ? nullptr : find_request(*projection, id);
}

}  // namespace

struct EmergencyRuntime::Impl {
  RuntimeOptions options{};
  std::unique_ptr<DurableStore> store{};
  mutable std::mutex mutex{};
  bool closed{false};
  bool degraded{false};
  Status degraded_cause{};
  RuntimeStats stats{};
  IMitigationTransport* transport{nullptr};

  [[nodiscard]] DomainState& live() { return store->snapshot().live; }
  [[nodiscard]] const DomainState& live() const { return store->snapshot().live; }
  [[nodiscard]] StoreSnapshot& snapshot() { return store->snapshot(); }

  [[nodiscard]] Status require_usable() const {
    if (g_dispatch_depth > 0) {
      return Status::error(
          StatusCode::ReentrancyRefused,
          "the runtime was re-entered from a mitigation transport callback");
    }
    if (closed) {
      return Status::error(StatusCode::RuntimeClosed, "the runtime has been shut down");
    }
    if (degraded) {
      return degraded_status(degraded_cause);
    }
    return Status::success();
  }

  [[nodiscard]] AuthorityView view() const {
    AuthorityView view;
    view.epoch = live().epoch;
    view.incarnation = live().incarnation;
    view.revision = live().revision;
    view.incident = live().current_incident;
    view.generation = live().current_generation;
    view.incident_present = live().current_incident.is_set();
    return view;
  }

  [[nodiscard]] Status check_token(const AuthorityToken& token) const {
    return check_authority(token, view());
  }

  [[nodiscard]] JournalEntry make_entry(JournalKind kind, JournalPayload payload, Timestamp at,
                                        IncidentId incident) const {
    JournalEntry entry;
    entry.sequence = live().journal_sequence.next();
    entry.at = at;
    entry.epoch = live().epoch;
    entry.incarnation = live().incarnation;
    entry.revision_before = live().revision;
    entry.revision_after = live().revision.next();
    entry.incident = incident;
    entry.kind = kind;
    entry.payload = std::move(payload);
    return entry;
  }

  // Applies an entry to the live state and, when it changed something, records
  // it in the journal and retires entries past the retention window into the
  // checkpoint. Returns the outcome; a no-op tick is simply not recorded.
  [[nodiscard]] Result<ApplyOutcome> apply_and_record(JournalEntry entry) {
    DomainState& state = live();
    auto outcome = apply(state, entry);
    if (!outcome.ok()) {
      return outcome.status();
    }
    if (!outcome.value().state_changed) {
      return outcome;
    }
    record(std::move(entry));
    return outcome;
  }

  void record(JournalEntry entry) {
    StoreSnapshot& snapshot_ref = snapshot();
    snapshot_ref.journal.push_back(std::move(entry));
    const std::size_t limit = snapshot_ref.live.policy.bounds.max_journal_entries;
    while (snapshot_ref.journal.size() > limit) {
      const JournalEntry front = snapshot_ref.journal.front();
      auto advanced = apply(snapshot_ref.checkpoint, front);
      if (!advanced.ok()) {
        // The checkpoint cannot be advanced; keep the entry and try again on
        // the next record rather than silently losing history.
        break;
      }
      snapshot_ref.journal.erase(snapshot_ref.journal.begin());
      ++snapshot_ref.journal_dropped;
      ++stats.journal_dropped;
    }
  }

  [[nodiscard]] Status commit(Timestamp at) {
    if (degraded) {
      return degraded_status(degraded_cause);
    }
    Status status = store->Commit(at);
    if (!status.ok()) {
      degraded = true;
      degraded_cause = status;
      return status;
    }
    ++stats.commits;
    return Status::success();
  }
};

EmergencyRuntime::~EmergencyRuntime() = default;

Result<std::unique_ptr<EmergencyRuntime>> EmergencyRuntime::Open(const RuntimeOptions& options) {
  auto validated = validate_policy(options.policy);
  if (!validated.ok()) {
    return validated.status();
  }
  if (options.epoch.is_absent() || options.incarnation.is_absent()) {
    return Status::error(StatusCode::MissingAuthority,
                         "opening a runtime requires an explicit control epoch and controller "
                         "incarnation");
  }

  StoreOpenOptions store_options;
  store_options.root = options.store_root;
  store_options.durability = options.durability;
  store_options.create_if_missing = options.create_if_missing;

  auto store = DurableStore::Open(store_options);
  if (!store.ok()) {
    return store.status();
  }

  auto runtime = std::unique_ptr<EmergencyRuntime>(new EmergencyRuntime());
  runtime->impl_ = std::make_unique<Impl>();
  Impl& impl = *runtime->impl_;
  impl.options = options;
  impl.options.policy = options.policy;
  impl.store = std::move(store).value();
  impl.transport = options.transport;

  StoreSnapshot& snapshot_ref = impl.snapshot();
  DomainState& state = snapshot_ref.live;

  if (impl.store->open_report().created) {
    DomainState initial;
    initial.incarnation = options.incarnation;
    initial.epoch = options.epoch;
    initial.policy = options.policy;
    initial.relaxation = options.relaxation;
    snapshot_ref.checkpoint = initial;
    snapshot_ref.live = initial;
    const Status status = impl.commit(Timestamp{});
    if (!status.ok()) {
      return status;
    }
    return runtime;
  }

  // A store that already holds durable state is fenced before it is used.
  if (options.epoch < state.epoch) {
    return Status::error(StatusCode::StaleEpoch,
                         "the controller epoch is older than the durable authority epoch")
        .with_context("durable epoch " + std::to_string(state.epoch.value()));
  }
  const Timestamp recovered_at = state.last_input_at;
  if (options.epoch > state.epoch) {
    AuthorityRolledOverPayload payload;
    payload.incarnation = options.incarnation;
    payload.epoch = options.epoch;
    auto applied = impl.apply_and_record(
        impl.make_entry(JournalKind::AuthorityRolledOver, std::move(payload), recovered_at,
                        IncidentId{}));
    if (!applied.ok()) {
      return applied.status();
    }
  } else if (!(options.incarnation == state.incarnation)) {
    return Status::error(StatusCode::StaleIncarnation,
                         "a new controller incarnation must roll the control epoch explicitly");
  }

  if (!same_policy(state.policy, state.relaxation, options.policy, options.relaxation)) {
    if (!options.adopt_policy_changes) {
      return Status::error(StatusCode::PreconditionFailed,
                           "the durable store was written under a different policy generation");
    }
    PolicyAdoptedPayload payload;
    payload.policy = options.policy;
    payload.relaxation = options.relaxation;
    auto applied = impl.apply_and_record(
        impl.make_entry(JournalKind::PolicyAdopted, std::move(payload), recovered_at, IncidentId{}));
    if (!applied.ok()) {
      return applied.status();
    }
  }

  StoreRecoveredPayload recovered;
  recovered.rollover = options.epoch > state.epoch;
  recovered.previous_incarnation = state.incarnation;
  recovered.previous_epoch = state.epoch;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::StoreRecovered, std::move(recovered), recovered_at, IncidentId{}));
  if (!applied.ok()) {
    return applied.status();
  }
  ++impl.stats.store_recoveries;
  const Status status = impl.commit(recovered_at);
  if (!status.ok()) {
    return status;
  }
  return runtime;
}

AuthorityToken EmergencyRuntime::Authority() const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  AuthorityToken token;
  token.incident = impl.live().current_incident;
  token.generation = impl.live().current_generation;
  token.epoch = impl.live().epoch;
  token.incarnation = impl.live().incarnation;
  token.expected_revision = impl.live().revision;
  return token;
}

bool EmergencyRuntime::closed() const noexcept {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  return impl.closed;
}

VoidResult EmergencyRuntime::RegisterZone(const AuthorityToken& token, ThermalZoneRef zone) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  if (!zone.valid()) {
    return Status::error(StatusCode::EmptyField, "thermal zone reference is empty");
  }
  ZoneRegisteredPayload payload;
  payload.zone = zone;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::ZoneRegistered, std::move(payload), impl.live().last_input_at,
                      IncidentId{}));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(impl.live().last_input_at);
}

VoidResult EmergencyRuntime::RegisterProbe(const AuthorityToken& token, ThermalZoneRef zone,
                                           ThermalProbeRef probe) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  if (!zone.valid() || !probe.valid()) {
    return Status::error(StatusCode::EmptyField, "zone and probe references must both be present");
  }
  ProbeRegisteredPayload payload;
  payload.zone = zone;
  payload.probe = probe;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::ProbeRegistered, std::move(payload), impl.live().last_input_at,
                      IncidentId{}));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(impl.live().last_input_at);
}

VoidResult EmergencyRuntime::RegisterTargetBinding(const AuthorityToken& token,
                                                   const TargetBinding& binding) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  if (!binding.zone.valid()) {
    return Status::error(StatusCode::EmptyField, "target binding requires a thermal zone");
  }
  TargetBindingRegisteredPayload payload;
  payload.zone = binding.zone;
  payload.power_domain = binding.power_domain;
  payload.equipment = binding.equipment;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::TargetBindingRegistered, std::move(payload),
                      impl.live().last_input_at, IncidentId{}));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(impl.live().last_input_at);
}

Result<ObligationId> EmergencyRuntime::RegisterObligation(const AuthorityToken& token,
                                                          ProtectedObligation obligation) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  const Bounds& bounds = impl.live().policy.bounds;
  auto validated = validate_obligation(obligation, bounds);
  if (!validated.ok()) {
    return validated;
  }
  if (impl.live().obligations.size() >= bounds.max_obligations) {
    return Status::error(StatusCode::ResourceExhausted, "the obligation table is full");
  }
  obligation.id = impl.live().next_obligation_id;
  const ObligationId assigned = obligation.id;
  ObligationRegisteredPayload payload;
  payload.obligation = obligation;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::ObligationRegistered, std::move(payload),
                      impl.live().last_input_at, IncidentId{}));
  if (!applied.ok()) {
    return applied.status();
  }
  Status status = impl.commit(impl.live().last_input_at);
  if (!status.ok()) {
    return status;
  }
  ++impl.stats.obligations_registered;
  return assigned;
}

VoidResult EmergencyRuntime::ReportObligationStatus(const AuthorityToken& token, ObligationId id,
                                                    ObligationStatus status,
                                                    ExternalAuthorityRef source,
                                                    Timestamp status_at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  if (find_obligation(impl.live(), id) == nullptr) {
    return Status::error(StatusCode::ObligationUnknown, "obligation is not registered")
        .with_context(std::to_string(id.value()));
  }
  if (status_at < impl.live().last_input_at) {
    return Status::error(StatusCode::EvidenceOutOfOrder,
                         "obligation status report predates the last accepted input");
  }
  ObligationStatusPayload payload;
  payload.obligation = id;
  payload.status = status;
  payload.status_at = status_at;
  payload.source = source;
  auto applied = impl.apply_and_record(impl.make_entry(JournalKind::ObligationStatusReported,
                                                       std::move(payload), status_at, IncidentId{}));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(status_at);
}

VoidResult EmergencyRuntime::AdmitSample(const AuthorityToken& token, const ThermalSample& sample) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  const DomainState& state = impl.live();
  auto shape = validate_sample_shape(sample, state.policy);
  if (!shape.ok()) {
    ++impl.stats.evidence_rejected;
    return shape;
  }
  const ZoneSlot* zone = find_zone(state, sample.zone);
  if (zone == nullptr) {
    ++impl.stats.evidence_rejected;
    return Status::error(StatusCode::EvidenceUnknownTarget, "thermal zone is not registered")
        .with_context(sample.zone.text());
  }
  const ProbeSlot* probe = find_probe(*zone, sample.probe);
  if (probe == nullptr) {
    ++impl.stats.evidence_rejected;
    return Status::error(StatusCode::EvidenceUnknownTarget, "probe is not registered")
        .with_context(sample.probe.text());
  }
  if (probe->present && probe->last_sequence == sample.sequence) {
    ++impl.stats.evidence_rejected;
    return Status::error(StatusCode::EvidenceDuplicate,
                         "a sample with this sequence was already accepted")
        .with_context(sample.probe.text());
  }
  if (probe->present && !(probe->last_sequence < sample.sequence)) {
    ++impl.stats.evidence_rejected;
    return Status::error(StatusCode::EvidenceOutOfOrder,
                         "sample sequence does not advance the probe stream")
        .with_context(sample.probe.text());
  }
  if (sample.received_at < state.last_input_at) {
    ++impl.stats.evidence_rejected;
    return Status::error(StatusCode::EvidenceOutOfOrder,
                         "sample receipt predates the last accepted input")
        .with_context(sample.probe.text());
  }
  EvidenceAdmittedPayload payload;
  payload.sample = sample;
  auto applied = impl.apply_and_record(impl.make_entry(JournalKind::EvidenceAdmitted,
                                                       std::move(payload), sample.received_at,
                                                       IncidentId{}));
  if (!applied.ok()) {
    ++impl.stats.evidence_rejected;
    return applied.status();
  }
  ++impl.stats.evidence_admitted;
  return impl.commit(sample.received_at);
}

Result<TickReport> EmergencyRuntime::Tick(const AuthorityToken& token, Timestamp now) {
  Impl& impl = *impl_;
  std::vector<MitigationRequest> to_dispatch;
  TickReport report;
  report.evaluated_at = now;
  IncidentId incident_id{};

  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    auto usable = impl.require_usable();
    if (!usable.ok()) {
      return usable;
    }
    auto authority = impl.check_token(token);
    if (!authority.ok()) {
      return authority;
    }
    DomainState& state = impl.live();
    if (state.zones.empty()) {
      return Status::error(StatusCode::PreconditionFailed,
                           "no thermal zone is registered, so no decision can be taken");
    }
    if (now < state.last_input_at) {
      return Status::error(StatusCode::PreconditionFailed,
                           "the supplied instant precedes the last accepted input");
    }

    ++impl.stats.ticks;

    // 1. Open an incident when the evidence justifies one.
    const IncidentProjection* current = find_incident(state, state.current_incident);
    if (current == nullptr || current->closed) {
      const EvidenceAssessment assessment = assess_state(state, now);
      const Severity justified = justified_severity(assessment, state.policy);
      if (severity_ordinal(justified) >= severity_ordinal(Severity::Advisory)) {
        // The trigger zone is the hottest zone with a current reading. A zone
        // whose other probes are stale still provides a current reading from
        // the probes that are reporting, and losing one probe must not hide an
        // excursion that the remaining probes still measure.
        const ZoneAssessment* worst = nullptr;
        for (const ZoneAssessment& zone : assessment.zones) {
          if (!zone.has_temperature) {
            continue;
          }
          if (worst == nullptr || worst->max_temperature < zone.max_temperature) {
            worst = &zone;
          }
        }
        if (worst == nullptr) {
          return Status::error(StatusCode::EvidenceNotCurrent,
                               "an incident cannot be opened without current zone evidence");
        }
        IncidentOpenedPayload payload;
        payload.trigger_zone = worst->zone;
        payload.initial = justified;
        auto applied = impl.apply_and_record(
            impl.make_entry(JournalKind::IncidentOpened, std::move(payload), now, IncidentId{}));
        if (!applied.ok()) {
          return applied.status();
        }
      }
    }

    // 2. Evaluate the tick.
    TickPayload tick_payload;
    tick_payload.evaluated_zones = static_cast<std::uint32_t>(state.zones.size());
    incident_id = state.current_incident;
    JournalEntry tick_entry =
        impl.make_entry(JournalKind::Tick, std::move(tick_payload), now, incident_id);
    auto tick_outcome = apply(state, tick_entry);
    if (!tick_outcome.ok()) {
      return tick_outcome.status();
    }
    report.state_changed = tick_outcome.value().state_changed;
    report.severity_before = tick_outcome.value().severity_before;
    report.severity_after = tick_outcome.value().severity_after;
    report.lifecycle_before = tick_outcome.value().lifecycle_before;
    report.lifecycle_after = tick_outcome.value().lifecycle_after;
    report.requests_expired = tick_outcome.value().requests_expired;
    report.requests_failed = tick_outcome.value().requests_failed;
    report.requests_abandoned = tick_outcome.value().requests_abandoned;
    if (tick_outcome.value().state_changed) {
      impl.record(std::move(tick_entry));
      ++impl.stats.ticks_changed;
      if (severity_ordinal(report.severity_after) > severity_ordinal(report.severity_before)) {
        ++impl.stats.escalations;
      }
      if (severity_ordinal(report.severity_after) < severity_ordinal(report.severity_before)) {
        ++impl.stats.deescalations;
      }
    }

    // 3. Issue the requests the plan requires.
    IncidentProjection* incident = find_incident(state, state.current_incident);
    if (incident != nullptr && !incident->closed) {
      const EscalationPlan plan = plan_escalation(state, now);
      for (const PlannedRequest& planned : plan.requests) {
        const Fingerprint fingerprint =
            fingerprint_of_request_fields(incident->id, incident->generation, state.epoch,
                                          planned.cls, planned.target, planned.intensity);
        const IdempotencyKey key = IdempotencyKey::from_fingerprint(fingerprint);
        bool has_open = false;
        bool has_verified = false;
        bool has_open_other_fingerprint = false;
        MitigationRequestId open_other{};
        std::uint32_t attempts = 0;
        for (const MitigationRecord& record : incident->requests) {
          if (record.request.cls != planned.cls || !(record.request.target == planned.target)) {
            continue;
          }
          ++attempts;
          if (record.state == MitigationState::Verified) {
            has_verified = true;
          } else if (!record.is_terminal()) {
            if (record.request.key == key) {
              has_open = true;
            } else {
              has_open_other_fingerprint = true;
              open_other = record.request.id;
            }
          }
        }
        if (has_open || has_verified) {
          continue;
        }
        if (attempts >= kMaxRequestAttempts) {
          continue;
        }
        if (has_open_other_fingerprint) {
          RequestSupersededPayload supersede;
          supersede.request = open_other;
          supersede.reason = SupersedeReason::PlanChanged;
          auto applied = impl.apply_and_record(impl.make_entry(
              JournalKind::RequestSuperseded, std::move(supersede), now, incident->id));
          if (!applied.ok()) {
            return applied.status();
          }
        }

        MitigationRequest request;
        request.id = state.next_request_id;
        request.attempt = state.next_attempt_id;
        request.command = state.next_command_id;
        request.incident = incident->id;
        request.incident_generation = incident->generation;
        request.epoch = state.epoch;
        request.incarnation = state.incarnation;
        request.planned_revision = state.revision.next();
        request.cls = planned.cls;
        request.target = planned.target;
        request.intensity = planned.intensity;
        request.reason = planned.reason;
        request.evidence_refs = planned.evidence_refs;
        request.fingerprint = fingerprint;
        request.key = key;
        request.created_at = now;
        request.expires_at = now.checked_add(state.policy.request_validity).value_or(now);
        request.has_expiry = true;
        request.verification_deadline =
            now.checked_add(state.policy.verification_deadline).value_or(now);
        request.has_verification_deadline = true;

        RequestsIssuedPayload issued;
        issued.requests.push_back(request);
        auto applied = impl.apply_and_record(impl.make_entry(
            JournalKind::RequestsIssued, std::move(issued), now, incident->id));
        if (!applied.ok()) {
          return applied.status();
        }
        to_dispatch.push_back(request);
        ++report.requests_issued;
        ++impl.stats.requests_issued;
      }
    }

    const Status status = impl.commit(now);
    if (!status.ok()) {
      return status;
    }
    report.revision = impl.live().revision;
    report.eligibility = evaluate_recovery(impl.live(), now, false);
  }

  // 4. Dispatch outside the lock: the persisted intent is the write-ahead
  // record, and no lock is held while an adjacent authority is called.
  if (!to_dispatch.empty()) {
    std::vector<DispatchOutcomeRecord> outcomes;
    outcomes.reserve(to_dispatch.size());
    for (const MitigationRequest& request : to_dispatch) {
      if (impl.transport == nullptr) {
        // Nothing was delivered, so nothing is acknowledged: the request stays
        // planned, will expire at its validity deadline, and the requirement
        // stays unmet, which continues escalation. Pretending it was
        // acknowledged would be a fabricated external effect.
        ++report.requests_indeterminate;
        ++impl.stats.dispatch_indeterminate;
        continue;
      }
      DispatchOutcomeRecord record;
      record.request = request;
      DispatchScope scope;
      record.result = impl.transport->Dispatch(request);
      outcomes.push_back(std::move(record));
    }

    std::lock_guard<std::mutex> guard(impl.mutex);
    if (!impl.degraded && !impl.closed) {
      Timestamp recorded_at = impl.live().last_input_at;
      for (DispatchOutcomeRecord& outcome : outcomes) {
        const MitigationRecord* existing =
            find_request_in(impl.live(), incident_id, outcome.request.id);
        if (existing == nullptr || existing->state != MitigationState::Planned) {
          continue;  // superseded or resolved before the answer arrived
        }
        recorded_at = impl.live().last_input_at;
        const JournalEntry entry = [&]() {
          switch (outcome.result.outcome) {
            case DispatchOutcome::Accepted: {
              RequestAcknowledgedPayload payload;
              payload.request = outcome.request.id;
              payload.external_ref = outcome.result.external_ref;
              return impl.make_entry(JournalKind::RequestAcknowledged, std::move(payload),
                                     recorded_at, incident_id);
            }
            case DispatchOutcome::Refused: {
              RequestFailedPayload payload;
              payload.request = outcome.request.id;
              payload.failure = FailureCode::TransportRefused;
              payload.transport_code = outcome.result.code;
              return impl.make_entry(JournalKind::RequestFailed, std::move(payload), recorded_at,
                                     incident_id);
            }
            case DispatchOutcome::Unsupported: {
              RequestFailedPayload payload;
              payload.request = outcome.request.id;
              payload.failure = FailureCode::Unsupported;
              payload.transport_code = outcome.result.code;
              return impl.make_entry(JournalKind::RequestFailed, std::move(payload), recorded_at,
                                     incident_id);
            }
            case DispatchOutcome::Error:
            default: {
              RequestFailedPayload payload;
              payload.request = outcome.request.id;
              payload.failure = FailureCode::TransportError;
              payload.transport_code = outcome.result.code;
              return impl.make_entry(JournalKind::RequestFailed, std::move(payload), recorded_at,
                                     incident_id);
            }
          }
        }();
        auto applied = impl.apply_and_record(entry);
        if (!applied.ok()) {
          continue;
        }
        if (outcome.result.outcome == DispatchOutcome::Accepted) {
          ++report.requests_dispatched;
          if (impl.transport == nullptr) {
            ++report.requests_indeterminate;
            ++impl.stats.dispatch_indeterminate;
          } else {
            ++impl.stats.requests_dispatched;
          }
        } else {
          ++report.requests_failed;
          ++impl.stats.dispatch_failures;
        }
      }
      const Status status = impl.commit(recorded_at);
      if (!status.ok()) {
        return status;
      }
      report.revision = impl.live().revision;
      IncidentId current = impl.live().current_incident;
      if (current.is_set()) {
        report.eligibility = evaluate_recovery(impl.live(), now, false);
      }
    }
  }

  return report;
}

RecoveryEligibility EmergencyRuntime::EvaluateRecovery(Timestamp now, bool for_closure) const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  return evaluate_recovery(impl.live(), now, for_closure);
}

Result<EscalationPlan> EmergencyRuntime::Plan(Timestamp now) const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  return plan_escalation(impl.live(), now);
}

VoidResult EmergencyRuntime::BeginRecovery(const AuthorityToken& token, OperatorRef operator_ref,
                                           Timestamp now) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  const IncidentProjection* incident = find_incident(impl.live(), impl.live().current_incident);
  if (incident == nullptr) {
    return Status::error(StatusCode::NoActiveIncident, "no incident is open");
  }
  if (incident->lifecycle != Lifecycle::Stabilizing) {
    return Status::error(StatusCode::IllegalTransition,
                         "recovery can only begin from the stabilizing lifecycle")
        .with_context(std::string(lifecycle_name(incident->lifecycle)));
  }
  if (now < impl.live().last_input_at) {
    return Status::error(StatusCode::PreconditionFailed,
                         "the supplied instant precedes the last accepted input");
  }
  const RecoveryEligibility eligibility = evaluate_recovery(impl.live(), now, false);
  if (!recovery_preconditions_hold(eligibility) || !incident->margin_ok) {
    return Status::error(StatusCode::RecoveryNotEligible,
                         "recovery preconditions do not hold under current evidence")
        .with_context(eligibility.summarize());
  }
  RecoveryBegunPayload payload;
  payload.operator_ref = operator_ref;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::RecoveryBegun, std::move(payload), now, incident->id));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(now);
}

VoidResult EmergencyRuntime::CloseIncident(const AuthorityToken& token, OperatorRef operator_ref,
                                           Disposition disposition, Timestamp now) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  const IncidentProjection* incident = find_incident(impl.live(), impl.live().current_incident);
  if (incident == nullptr) {
    return Status::error(StatusCode::NoActiveIncident, "no incident is open");
  }
  if (now < impl.live().last_input_at) {
    return Status::error(StatusCode::PreconditionFailed,
                         "the supplied instant precedes the last accepted input");
  }
  const RecoveryEligibility eligibility = evaluate_recovery(impl.live(), now, true);
  if (disposition == Disposition::Recovered && !eligibility.eligible) {
    return Status::error(StatusCode::GateNotSatisfied,
                         "the incident cannot be closed as recovered: gates do not pass")
        .with_context(eligibility.summarize());
  }
  if (disposition == Disposition::Mitigated) {
    const GateResult resolution = eligibility.result_of(RecoveryGate::MitigationResolution);
    const GateResult interlock = eligibility.result_of(RecoveryGate::InterlockIntegrity);
    if (resolution == GateResult::Failed || resolution == GateResult::Unknown ||
        interlock == GateResult::Failed || interlock == GateResult::Unknown) {
      return Status::error(StatusCode::GateNotSatisfied,
                           "the incident cannot be closed as mitigated: mitigation or obligation "
                           "gates do not pass")
          .with_context(eligibility.summarize());
    }
  }
  IncidentClosedPayload payload;
  payload.operator_ref = operator_ref;
  payload.disposition = disposition;
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::IncidentClosed, std::move(payload), now, incident->id));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(now);
}

Result<MitigationRequest> EmergencyRuntime::IssueRequest(const AuthorityToken& token,
                                                         const MitigationRequestDraft& draft,
                                                         const IssueOptions& options,
                                                         Timestamp now) {
  Impl& impl = *impl_;

  // Phase 1 (under the state lock): validate, record the intent durably, and
  // commit it. Phase 2 (outside the lock) dispatches to the adjacent authority
  // and then records the answer. The persisted intent is therefore always the
  // write-ahead record for the external request.
  struct Prepared {
    MitigationRequest request;
    bool needs_dispatch{false};
  };

  Prepared prepared_result;
  IncidentId incident_id{};
  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    auto prepared = [&]() -> Result<Prepared> {
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  DomainState& state = impl.live();
  IncidentProjection* incident = find_incident(state, state.current_incident);
  if (incident == nullptr) {
    return Status::error(StatusCode::NoActiveIncident, "no incident is open");
  }
  if (incident->closed) {
    return Status::error(StatusCode::IncidentClosed, "the incident is closed");
  }
  if (!draft.target.valid()) {
    return Status::error(StatusCode::EmptyField, "the request target is empty");
  }
  if (draft.target.kind() != mitigation_target_kind(draft.cls)) {
    return Status::error(StatusCode::RequestTargetMismatch,
                         "the request target kind does not match the mitigation class")
        .with_context(std::string(mitigation_class_name(draft.cls)));
  }
  if (draft.evidence_refs.size() > state.policy.bounds.max_evidence_refs) {
    return Status::error(StatusCode::TooManyElements, "too many evidence references");
  }
  const ProtectedObligation* blocking =
      find_blocking_obligation(state, *incident, draft.target, draft.cls);
  if (blocking != nullptr) {
    return Status::error(StatusCode::ProtectedObligationViolation,
                         "a protected obligation forbids this mitigation on this target")
        .with_context(blocking->ref);
  }

  const Fingerprint fingerprint = fingerprint_of_request_fields(
      incident->id, incident->generation, state.epoch, draft.cls, draft.target, draft.intensity);
  const IdempotencyKey computed_key = IdempotencyKey::from_fingerprint(fingerprint);
  const IdempotencyKey key = options.has_client_key ? options.client_key : computed_key;

  for (const MitigationRecord& record : incident->requests) {
    if (!(record.request.key == key)) {
      continue;
    }
    if (!(record.request.fingerprint == fingerprint)) {
      return Status::error(StatusCode::IdempotencyConflict,
                           "the idempotency key was already used for a different request")
          .with_context(key.to_hex());
    }
    if (record.is_terminal() && record.state != MitigationState::Verified) {
      break;  // a new attempt may be issued after a terminal failure
    }
    // A retry of the same semantics replays the recorded request and must not
    // reach the adjacent authority a second time.
    ++impl.stats.deduped_retries;
    Prepared replay;
    replay.request = record.request;
    replay.needs_dispatch = false;
    return replay;
  }

  if (incident->requests.size() >= state.policy.bounds.max_requests_per_incident) {
    return Status::error(StatusCode::ResourceExhausted,
                         "the mitigation request table for the incident is full");
  }

  MitigationRequest request;
  request.id = state.next_request_id;
  request.attempt = state.next_attempt_id;
  request.command = state.next_command_id;
  request.incident = incident->id;
  request.incident_generation = incident->generation;
  request.epoch = state.epoch;
  request.incarnation = state.incarnation;
  request.planned_revision = state.revision.next();
  request.cls = draft.cls;
  request.target = draft.target;
  request.intensity = draft.intensity;
  request.reason = draft.reason;
  request.evidence_refs = draft.evidence_refs;
  request.fingerprint = fingerprint;
  request.key = key;
  request.created_at = now;
  request.has_expiry = options.has_expiry;
  request.expires_at = options.has_expiry
                           ? options.expires_at
                           : now.checked_add(state.policy.request_validity).value_or(now);
  request.has_verification_deadline = options.has_verification_deadline;
  request.verification_deadline =
      options.has_verification_deadline
          ? options.verification_deadline
          : now.checked_add(state.policy.verification_deadline).value_or(now);

  RequestsIssuedPayload payload;
  payload.requests.push_back(request);
  auto applied = impl.apply_and_record(
      impl.make_entry(JournalKind::RequestsIssued, std::move(payload), now, incident->id));
  if (!applied.ok()) {
    return applied.status();
  }
  Status status = impl.commit(now);
  if (!status.ok()) {
    return status;
  }
  ++impl.stats.requests_issued;
  Prepared outcome;
  outcome.request = request;
  outcome.needs_dispatch = true;
  return outcome;
    };
    auto result = prepared();
    if (!result.ok()) {
      return result.status();
    }
    prepared_result = std::move(result).value();
    incident_id = prepared_result.request.incident;
  }

  const MitigationRequest issued = prepared_result.request;
  if (!prepared_result.needs_dispatch) {
    return issued;
  }

  // Phase 2: dispatch outside the lock, then record the answer under it. With
  // no transport configured nothing is delivered and nothing is acknowledged.
  if (impl.transport == nullptr) {
    std::lock_guard<std::mutex> guard(impl.mutex);
    ++impl.stats.dispatch_indeterminate;
    return issued;
  }
  Status commit_status;
  DispatchResult dispatch;
  {
    DispatchScope scope;
    dispatch = impl.transport->Dispatch(issued);
  }

  {
    std::lock_guard<std::mutex> guard(impl.mutex);
    const MitigationRecord* existing = find_request_in(impl.live(), incident_id, issued.id);
    if (existing != nullptr && existing->state == MitigationState::Planned) {
      const Timestamp recorded_at = impl.live().last_input_at;
      JournalEntry entry = [&]() {
        switch (dispatch.outcome) {
          case DispatchOutcome::Accepted: {
            RequestAcknowledgedPayload ack;
            ack.request = issued.id;
            ack.external_ref = dispatch.external_ref;
            return impl.make_entry(JournalKind::RequestAcknowledged, std::move(ack), recorded_at,
                                   incident_id);
          }
          case DispatchOutcome::Refused: {
            RequestFailedPayload failed;
            failed.request = issued.id;
            failed.failure = FailureCode::TransportRefused;
            failed.transport_code = dispatch.code;
            return impl.make_entry(JournalKind::RequestFailed, std::move(failed), recorded_at,
                                   incident_id);
          }
          case DispatchOutcome::Unsupported: {
            RequestFailedPayload failed;
            failed.request = issued.id;
            failed.failure = FailureCode::Unsupported;
            failed.transport_code = dispatch.code;
            return impl.make_entry(JournalKind::RequestFailed, std::move(failed), recorded_at,
                                   incident_id);
          }
          case DispatchOutcome::Error:
          default: {
            RequestFailedPayload failed;
            failed.request = issued.id;
            failed.failure = FailureCode::TransportError;
            failed.transport_code = dispatch.code;
            return impl.make_entry(JournalKind::RequestFailed, std::move(failed), recorded_at,
                                   incident_id);
          }
        }
      }();
      auto applied_answer = impl.apply_and_record(std::move(entry));
      if (!applied_answer.ok()) {
        return applied_answer.status();
      }
      commit_status = impl.commit(recorded_at);
      if (!commit_status.ok()) {
        return commit_status;
      }
      if (dispatch.outcome == DispatchOutcome::Accepted) {
        if (impl.transport == nullptr) {
          ++impl.stats.dispatch_indeterminate;
        } else {
          ++impl.stats.requests_dispatched;
        }
      } else {
        ++impl.stats.dispatch_failures;
      }
    }
  }
  return issued;
}

VoidResult EmergencyRuntime::AcknowledgeRequest(const AuthorityToken& token, MitigationRequestId id,
                                                ExternalEvidenceRef external_ref, Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  RequestAcknowledgedPayload payload;
  payload.request = id;
  payload.external_ref = external_ref;
  auto applied = impl.apply_and_record(impl.make_entry(JournalKind::RequestAcknowledged,
                                                       std::move(payload), at,
                                                       impl.live().current_incident));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(at);
}

VoidResult EmergencyRuntime::ObserveRequest(const AuthorityToken& token, MitigationRequestId id,
                                            ExternalEvidenceRef external_ref,
                                            ObservationSequence observation, Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  RequestObservedPayload payload;
  payload.request = id;
  payload.external_ref = external_ref;
  payload.observation = observation;
  auto applied = impl.apply_and_record(impl.make_entry(
      JournalKind::RequestObserved, std::move(payload), at, impl.live().current_incident));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(at);
}

VoidResult EmergencyRuntime::VerifyRequest(const AuthorityToken& token, MitigationRequestId id,
                                           ExternalEvidenceRef verification_ref, Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  RequestVerifiedPayload payload;
  payload.request = id;
  payload.verification_ref = verification_ref;
  auto applied = impl.apply_and_record(impl.make_entry(
      JournalKind::RequestVerified, std::move(payload), at, impl.live().current_incident));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(at);
}

VoidResult EmergencyRuntime::FailRequest(const AuthorityToken& token, MitigationRequestId id,
                                         FailureCode failure, std::int32_t transport_code,
                                         Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  if (failure == FailureCode::None) {
    return Status::error(StatusCode::InvalidArgument,
                         "a failure report must carry a failure code");
  }
  RequestFailedPayload payload;
  payload.request = id;
  payload.failure = failure;
  payload.transport_code = transport_code;
  auto applied = impl.apply_and_record(impl.make_entry(
      JournalKind::RequestFailed, std::move(payload), at, impl.live().current_incident));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(at);
}

VoidResult EmergencyRuntime::AbandonRequest(const AuthorityToken& token, MitigationRequestId id,
                                            Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  RequestAbandonedPayload payload;
  payload.request = id;
  auto applied = impl.apply_and_record(impl.make_entry(
      JournalKind::RequestAbandoned, std::move(payload), at, impl.live().current_incident));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(at);
}

VoidResult EmergencyRuntime::RelaxConstraint(const AuthorityToken& token, ObligationId id,
                                             MitigationClass cls, ExternalAuthorityRef authority_ref,
                                             Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  DomainState& state = impl.live();
  const ObligationSlot* slot = find_obligation(state, id);
  if (slot == nullptr) {
    return Status::error(StatusCode::ObligationUnknown, "obligation is not registered")
        .with_context(std::to_string(id.value()));
  }
  if (!authority_ref.valid()) {
    return Status::error(StatusCode::InvalidArgument,
                         "relaxing a constraint requires the authority that grants it");
  }
  if (!relaxation_allowed(state.relaxation, slot->obligation.protection, cls)) {
    return Status::error(StatusCode::ObligationNotRelaxable,
                         "the emergency authority may not relax this obligation")
        .with_context(slot->obligation.ref);
  }
  if (!obligation_forbids(slot->obligation, cls)) {
    return Status::error(StatusCode::InvalidArgument,
                         "the obligation does not constrain this mitigation class")
        .with_context(slot->obligation.ref);
  }
  IncidentProjection* incident = find_incident(state, state.current_incident);
  if (incident == nullptr) {
    return Status::error(StatusCode::NoActiveIncident, "no incident is open");
  }
  for (const RelaxationRecord& relaxation : incident->relaxations) {
    if (relaxation.obligation == id && relaxation.cls == cls && relaxation.active) {
      return Status::error(StatusCode::ObligationAlreadyRelaxed,
                           "the constraint is already relaxed")
          .with_context(slot->obligation.ref);
    }
  }
  ConstraintRelaxedPayload payload;
  payload.obligation = id;
  payload.cls = cls;
  payload.authority = authority_ref;
  auto applied = impl.apply_and_record(impl.make_entry(
      JournalKind::ConstraintRelaxed, std::move(payload), at, incident->id));
  if (!applied.ok()) {
    return applied.status();
  }
  Status status = impl.commit(at);
  if (!status.ok()) {
    return status;
  }
  ++impl.stats.relaxations;
  return Status::success();
}

VoidResult EmergencyRuntime::ReimposeConstraint(const AuthorityToken& token, ObligationId id,
                                                Timestamp at) {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  auto usable = impl.require_usable();
  if (!usable.ok()) {
    return usable;
  }
  auto authority = impl.check_token(token);
  if (!authority.ok()) {
    return authority;
  }
  DomainState& state = impl.live();
  if (find_obligation(state, id) == nullptr) {
    return Status::error(StatusCode::ObligationUnknown, "obligation is not registered")
        .with_context(std::to_string(id.value()));
  }
  IncidentProjection* incident = find_incident(state, state.current_incident);
  if (incident == nullptr) {
    return Status::error(StatusCode::NoActiveIncident, "no incident is open");
  }
  bool active = false;
  for (const RelaxationRecord& relaxation : incident->relaxations) {
    if (relaxation.obligation == id && relaxation.active) {
      active = true;
      break;
    }
  }
  if (!active) {
    return Status::error(StatusCode::ObligationNotRelaxed, "the constraint is not relaxed")
        .with_context(std::to_string(id.value()));
  }
  ConstraintReimposedPayload payload;
  payload.obligation = id;
  auto applied = impl.apply_and_record(impl.make_entry(
      JournalKind::ConstraintReimposed, std::move(payload), at, incident->id));
  if (!applied.ok()) {
    return applied.status();
  }
  return impl.commit(at);
}

ReadModel build_read_model(const DomainState& state, const StoreSnapshot& snapshot,
                                  Timestamp now, const RuntimeStats& stats) {
  ReadModel model;
  model.incarnation = state.incarnation;
  model.epoch = state.epoch;
  model.revision = state.revision;
  model.commit_sequence = snapshot.commit_sequence;
  model.store_generation = snapshot.store_generation;
  model.recovered_from_store = state.recovered_from_store;
  model.policy = state.policy;
  model.relaxation = state.relaxation;
  model.zones = state.zones;
  model.obligations.reserve(state.obligations.size());
  for (const ObligationSlot& slot : state.obligations) {
    model.obligations.push_back(slot.obligation);
  }
  model.stats = stats;
  model.evidence = assess_state(state, now);

  const IncidentProjection* incident = find_incident(state, state.current_incident);
  if (incident != nullptr) {
    model.incident_open = !incident->closed;
    model.incident = incident->id;
    model.generation = incident->generation;
    model.severity = incident->severity;
    model.peak_severity = incident->peak_severity;
    model.lifecycle = incident->lifecycle;
    model.opened_at = incident->opened_at;
    model.closed_at = incident->closed_at;
    model.has_disposition = incident->has_disposition;
    model.disposition = incident->disposition;
    model.fenced_requests = incident->fenced_requests;
    model.requests = incident->requests;
    model.relaxations = incident->relaxations;
    model.transitions = incident->transitions;
    model.transitions_dropped = incident->transitions_dropped;
  }
  model.eligibility = evaluate_recovery(state, now, false);
  return model;
}

ReadModel EmergencyRuntime::Inspect(Timestamp now) const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  ReadModel model = build_read_model(impl.live(), impl.store->snapshot(), now, impl.stats);
  model.durability = impl.store->durability();
  const std::u8string root = impl.store->root().u8string();
  model.store_root = std::string(reinterpret_cast<const char*>(root.data()), root.size());
  return model;
}

Result<ReplayReport> EmergencyRuntime::VerifyReplay() const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  const StoreSnapshot& snapshot = impl.store->snapshot();
  ReplayReport report;
  report.journal_entries = static_cast<std::uint32_t>(snapshot.journal.size());
  report.checkpoint_revision = snapshot.checkpoint.revision;
  report.live_revision = snapshot.live.revision;
  report.incidents = static_cast<std::uint32_t>(snapshot.live.incidents.size());
  const IncidentProjection* incident =
      find_incident(snapshot.live, snapshot.live.current_incident);
  report.requests = incident != nullptr ? static_cast<std::uint32_t>(incident->requests.size()) : 0;
  ++impl.stats.replay_checks;

  auto reconstructed = replay(snapshot.checkpoint, snapshot.journal);
  if (!reconstructed.ok()) {
    ++impl.stats.replay_divergences;
    return Status::error(StatusCode::ReplayDivergence, "the journal does not replay cleanly")
        .with_context(reconstructed.status().to_string());
  }
  report.reconstructed_revision = reconstructed.value().revision;
  if (!states_encode_identically(reconstructed.value(), snapshot.live)) {
    ++impl.stats.replay_divergences;
    return Status::error(StatusCode::ReplayDivergence,
                         "replayed state is not identical to the live state");
  }
  report.verified = true;
  report.detail = "replay reproduced the live state exactly";
  return report;
}

StoreVerification EmergencyRuntime::VerifyStore() const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  return impl.store->Verify();
}

RuntimeStats EmergencyRuntime::Stats() const {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  return impl.stats;
}

const std::filesystem::path& EmergencyRuntime::store_root() const noexcept { return impl_->store->root(); }

void EmergencyRuntime::Shutdown() {
  Impl& impl = *impl_;
  std::lock_guard<std::mutex> guard(impl.mutex);
  if (impl.closed) {
    return;
  }
  impl.closed = true;
  impl.store->Close();
}

}  // namespace summon::tem
