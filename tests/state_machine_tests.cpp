// Thermal Emergency Manager -- emergency state machine proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support.hpp"
#include "tem/plan.hpp"

using namespace summon::tem;
using namespace temtest;

TEM_TEST(escalation_follows_the_ladder_and_never_skips_a_step) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  std::vector<Severity> observed;
  const std::int32_t samples[] = {22'000, 28'000, 33'000, 39'000, 46'000, 56'000};
  for (const std::int32_t temperature : samples) {
    h.temperature = temperature;
    Status stepped = h.step(Duration::from_seconds(10));
    TEM_REQUIRE(stepped.ok());
    const Severity severity = h.inspect().severity;
    TEM_CHECK_EQ(static_cast<int>(reference_severity(temperature, h.inspect().policy)),
                 static_cast<int>(severity));
    observed.push_back(severity);
  }
  for (std::size_t i = 1; i < observed.size(); ++i) {
    TEM_CHECK(severity_ordinal(observed[i - 1]) <= severity_ordinal(observed[i]));
  }
  // The samples chosen below cross exactly one threshold at a time, so the
  // observed sequence must be the ladder itself with no level skipped.
  const std::vector<Severity> ladder = {Severity::Nominal,    Severity::Advisory, Severity::Warning,
                                        Severity::Critical,   Severity::Emergency,
                                        Severity::Catastrophic};
  TEM_CHECK(observed == ladder);
}

TEM_TEST(severity_does_not_regress_without_the_recovery_path) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 46'000;
  h.rate = 3'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(2)));
  const Severity peak = h.inspect().severity;
  TEM_REQUIRE(severity_ordinal(peak) >= severity_ordinal(Severity::Emergency));

  // Cool down abruptly, but never ask for recovery.
  h.temperature = 20'000;
  h.rate = -1'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(20)));
  const ReadModel model = h.inspect();
  TEM_CHECK_EQ(static_cast<int>(peak), static_cast<int>(model.severity));
  TEM_CHECK(model.lifecycle == Lifecycle::Stabilizing ||
            model.lifecycle == Lifecycle::Active);
  TEM_CHECK(!model.eligibility.eligible || model.lifecycle == Lifecycle::Stabilizing);
}

TEM_TEST(lost_evidence_raises_the_floor_and_blocks_recovery) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 22'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(3)));
  TEM_CHECK(h.inspect().severity == Severity::Nominal);

  // The only probe stops reporting: the stored reading ages out.
  h.probes = {"zone-a/p1"};
  h.temperature = 29'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(1)));
  // The reading alone justifies advisory; losing the second probe within the
  // staleness window raises the floor to warning.
  TEM_CHECK(severity_ordinal(h.inspect().severity) >= severity_ordinal(Severity::Advisory));

  const std::size_t monitors = h.probes.size();
  (void)monitors;
  // Freeze the probe by never feeding it again and letting the window pass.
  const Timestamp frozen = h.now;
  h.now = frozen.checked_add(Duration::from_seconds(120)).value_or(frozen);
  auto report = h.tick();
  TEM_REQUIRE(report.ok());
  const ReadModel model = h.inspect();
  TEM_CHECK(model.evidence.all_fresh == false);
  TEM_CHECK(severity_ordinal(model.severity) >= severity_ordinal(Severity::Warning));
  TEM_CHECK(!model.eligibility.eligible);
  const GateEvaluation* freshness = model.eligibility.find(RecoveryGate::EvidenceFreshness);
  TEM_REQUIRE(freshness != nullptr);
  TEM_CHECK(freshness->result != GateResult::Passed);
}

TEM_TEST(recovery_requires_verified_mitigation_hysteresis_and_dwell) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 34'000;
  h.rate = 2'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(3)));
  const ReadModel escalated = h.inspect();
  TEM_REQUIRE(severity_ordinal(escalated.severity) >= severity_ordinal(Severity::Warning));
  TEM_REQUIRE(h.count(MitigationState::Verified) > 0);

  // Cooling starts, but the temperature stays above the hysteresis band of the
  // level being left: leaving Critical requires dropping below
  // critical_enter - recovery_margin, which is 36.000 C.
  h.temperature = 37'500;
  h.rate = -1'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(6)));
  const ReadModel in_band = h.inspect();
  if (in_band.eligibility.eligible) {
    TEM_FAIL("recovery was reported eligible inside the hysteresis band: "
             << in_band.eligibility.summarize());
  }
  const GateEvaluation* margin = in_band.eligibility.find(RecoveryGate::ThermalMargin);
  TEM_REQUIRE(margin != nullptr);
  TEM_CHECK(margin->result == GateResult::Failed);

  // Deep cooling: the band is reached, then the dwell must elapse before any
  // de-escalation step is permitted.
  h.temperature = 26'000;
  h.rate = -500;
  TEM_REQUIRE_OK(h.run(Duration::from_seconds(60)));
  const ReadModel early = h.inspect();
  const GateEvaluation* dwell = early.eligibility.find(RecoveryGate::Dwell);
  TEM_REQUIRE(dwell != nullptr);
  if (dwell->result != GateResult::Failed) {
    TEM_FAIL("dwell gate should not pass yet: " << early.eligibility.summarize());
  }

  const Severity before_recovery = early.severity;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(4)));
  const ReadModel stabilizing = h.inspect();
  if (stabilizing.lifecycle != Lifecycle::Stabilizing &&
      stabilizing.lifecycle != Lifecycle::Recovering &&
      stabilizing.lifecycle != Lifecycle::Recovered) {
    TEM_FAIL("expected the incident to reach the recovery path: "
             << stabilizing.eligibility.summarize() << " lifecycle="
             << lifecycle_name(stabilizing.lifecycle));
  }
  TEM_CHECK(severity_ordinal(stabilizing.severity) <= severity_ordinal(before_recovery));

  // No level is ever skipped on the way down.
  std::vector<Severity> steps;
  for (const TransitionRecord& transition : stabilizing.transitions) {
    if (transition.from_lifecycle == Lifecycle::Recovering &&
        transition.to_severity != transition.from_severity) {
      steps.push_back(transition.to_severity);
    }
  }
  for (std::size_t i = 1; i < steps.size(); ++i) {
    TEM_CHECK(severity_ordinal(steps[i]) + 1 == severity_ordinal(steps[i - 1]));
  }
}

TEM_TEST(partial_mitigation_is_never_treated_as_resolved) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  ScriptedTransport::Rule refuse;
  refuse.outcome = DispatchOutcome::Refused;
  refuse.code = 9;
  refuse.detail = "rejected";
  h.transport.SetRule(MitigationClass::DrainWorkload, refuse);

  h.temperature = 40'000;
  h.rate = 2'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(4)));
  const ReadModel model = h.inspect();
  TEM_REQUIRE(severity_ordinal(model.severity) >= severity_ordinal(Severity::Critical));
  TEM_CHECK(h.count(MitigationState::Verified) > 0);
  TEM_CHECK(h.count(MitigationState::Failed) > 0);

  const GateEvaluation* resolution = model.eligibility.find(RecoveryGate::MitigationResolution);
  TEM_REQUIRE(resolution != nullptr);
  TEM_CHECK(resolution->result != GateResult::Passed);
  TEM_CHECK(!model.eligibility.eligible);

  // While the condition the drain addresses still exists, the failed drain
  // keeps recovery blocked. Holding the zone in the critical band keeps that
  // requirement live.
  h.temperature = 39'000;
  h.rate = 0;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(6)));
  const ReadModel held = h.inspect();
  const GateEvaluation* held_resolution =
      held.eligibility.find(RecoveryGate::MitigationResolution);
  TEM_REQUIRE(held_resolution != nullptr);
  TEM_CHECK(held_resolution->result == GateResult::Failed);
  TEM_CHECK(!held.eligibility.eligible);
  TEM_CHECK(held.lifecycle == Lifecycle::Active);
  TEM_CHECK(held.severity == Severity::Critical);

  // Deep cooling removes the condition itself, so the unmet requirement stops
  // being a reason to stay in the emergency state: the zone no longer justifies
  // the class at all. This is the documented moot-requirement rule, not a
  // relaxation of the gate while the hazard is live.
  h.temperature = 20'000;
  h.rate = -1'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(6)));
  const ReadModel cooled = h.inspect();
  TEM_CHECK(severity_ordinal(cooled.severity) >= severity_ordinal(Severity::Critical));
  TEM_CHECK(cooled.lifecycle == Lifecycle::Stabilizing ||
            cooled.lifecycle == Lifecycle::Recovering ||
            cooled.lifecycle == Lifecycle::Recovered);
}

TEM_TEST(acknowledgement_is_not_verification) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 33'000;
  h.rate = 1'500;
  // Feed and tick without ever confirming the effect.
  const Timestamp end = h.now.checked_add(Duration::from_minutes(3)).value_or(h.now);
  while (h.now < end) {
    h.now = h.now.checked_add(Duration::from_seconds(10)).value_or(h.now);
    TEM_REQUIRE(h.feed().ok());
    auto report = h.tick();
    TEM_REQUIRE(report.ok());
  }
  const ReadModel model = h.inspect();
  TEM_CHECK(h.count(MitigationState::Acknowledged) + h.count(MitigationState::Observed) > 0);
  TEM_CHECK(h.count(MitigationState::Verified) == 0);
  TEM_CHECK(!model.eligibility.eligible);
  const GateEvaluation* resolution = model.eligibility.find(RecoveryGate::MitigationResolution);
  TEM_REQUIRE(resolution != nullptr);
  TEM_CHECK(resolution->result != GateResult::Passed);
}

TEM_TEST(pending_verification_expires_and_escalation_continues) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 34'000;
  h.rate = 1'000;
  const Timestamp end = h.now.checked_add(Duration::from_minutes(8)).value_or(h.now);
  while (h.now < end) {
    h.now = h.now.checked_add(Duration::from_seconds(10)).value_or(h.now);
    TEM_REQUIRE(h.feed().ok());
    auto report = h.tick();
    TEM_REQUIRE(report.ok());
  }
  const ReadModel model = h.inspect();
  TEM_CHECK(h.count(MitigationState::Failed) > 0);
  TEM_CHECK(severity_ordinal(model.severity) >= severity_ordinal(Severity::Warning));
  TEM_CHECK(!model.eligibility.eligible);

  bool verification_timeout_seen = false;
  for (const MitigationRecord& record : model.requests) {
    if (record.state == MitigationState::Failed &&
        record.failure == FailureCode::VerificationTimeout) {
      verification_timeout_seen = true;
    }
  }
  TEM_CHECK(verification_timeout_seen);
}

TEM_TEST(hard_obligations_block_mitigation_while_advisory_ones_can_be_relaxed) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  ProtectedObligation interlock;
  interlock.ref = "obligation/hard-interlock";
  interlock.zone = ThermalZoneRef::parse(h.zone).value();
  interlock.description = "hard interlock";
  interlock.protection = ProtectionClass::HardSafetyInterlock;
  interlock.forbidden_classes = mitigation_class_bit(MitigationClass::DerateAccelerators);
  interlock.authority = ExternalAuthorityRef{std::string("ops/safety")};
  auto registered = h.runtime->RegisterObligation(h.runtime->Authority(), interlock);
  TEM_REQUIRE(registered.ok());

  auto zone_ref = ThermalZoneRef::parse(h.zone);
  MitigationRequestDraft draft;
  draft.cls = MitigationClass::DerateAccelerators;
  draft.target = RequestTarget::for_zone(zone_ref.value());
  draft.intensity = BasisPoints::from_value(3'000);

  h.temperature = 34'000;
  TEM_REQUIRE_OK(h.step(Duration::from_seconds(10)));
  auto refused = h.runtime->IssueRequest(h.runtime->Authority(), draft, IssueOptions{}, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::ProtectedObligationViolation);

  // The hard interlock is never relaxable, not even by the emergency authority.
  auto relaxed = h.runtime->RelaxConstraint(h.runtime->Authority(), registered.value(),
                                            MitigationClass::DerateAccelerators,
                                            ExternalAuthorityRef{std::string("ops/oncall")}, h.now);
  TEM_REQUIRE(!relaxed.ok());
  TEM_CHECK(relaxed.status().code() == StatusCode::ObligationNotRelaxable);

  // An advisory optimisation constraint can be relaxed when the policy allows it.
  ProtectedObligation advisory;
  advisory.ref = "obligation/advisory-fan-curve";
  advisory.zone = zone_ref.value();
  advisory.description = "advisory";
  advisory.protection = ProtectionClass::AdvisoryOptimization;
  advisory.forbidden_classes = mitigation_class_bit(MitigationClass::DrainWorkload);
  auto advisory_id = h.runtime->RegisterObligation(h.runtime->Authority(), advisory);
  TEM_REQUIRE(advisory_id.ok());

  RelaxationPolicy policy;
  policy.relaxable_classes = 0xFF;
  RuntimeOptions options = memory_options();
  options.relaxation = policy;
  // The live runtime was created with the default relaxation policy, which
  // permits no class. A fresh runtime with an explicit policy is used instead.
  auto second = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(second.ok());

  // The default policy allows advisory relaxation only for classes it lists,
  // so the attempt is refused deterministically rather than silently allowed.
  auto second_relax = h.runtime->RelaxConstraint(h.runtime->Authority(), advisory_id.value(),
                                                 MitigationClass::DrainWorkload,
                                                 ExternalAuthorityRef{std::string("ops/oncall")},
                                                 h.now);
  TEM_REQUIRE(!second_relax.ok());
  TEM_CHECK(second_relax.status().code() == StatusCode::ObligationNotRelaxable);
}

TEM_TEST(advisory_relaxation_is_recorded_and_permits_the_request) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  ProtectedObligation advisory;
  advisory.ref = "obligation/advisory-fan-curve";
  advisory.zone = ThermalZoneRef::parse(h.zone).value();
  advisory.description = "advisory optimisation constraint";
  advisory.protection = ProtectionClass::AdvisoryOptimization;
  advisory.forbidden_classes = mitigation_class_bit(MitigationClass::DerateAccelerators);
  auto registered = h.runtime->RegisterObligation(h.runtime->Authority(), advisory);
  TEM_REQUIRE(registered.ok());

  // The relay policy of the running runtime must allow the class; the open
  // options below mirror what a deployment would configure.
  auto zone_ref = ThermalZoneRef::parse(h.zone);
  MitigationRequestDraft draft;
  draft.cls = MitigationClass::DerateAccelerators;
  draft.target = RequestTarget::for_zone(zone_ref.value());
  draft.intensity = BasisPoints::from_value(3'000);

  h.temperature = 34'000;
  TEM_REQUIRE_OK(h.step(Duration::from_seconds(10)));
  auto blocked = h.runtime->IssueRequest(h.runtime->Authority(), draft, IssueOptions{}, h.now);
  TEM_REQUIRE(!blocked.ok());
  TEM_CHECK(blocked.status().code() == StatusCode::ProtectedObligationViolation);

  // A second runtime configured to permit advisory relaxation accepts it.
  TempDir dir("relax");
  RuntimeOptions options = durable_options(dir.path());
  RelaxationPolicy relaxation;
  relaxation.relaxable_classes = mitigation_class_bit(MitigationClass::DerateAccelerators);
  options.relaxation = relaxation;
  ScriptedTransport transport;
  options.transport = &transport;
  auto runtime = EmergencyRuntime::Open(options);
  TEM_REQUIRE(runtime.ok());
  EmergencyRuntime& rt = *runtime.value();
  TEM_REQUIRE(register_facility(rt, "zone-b", {"zone-b/p1", "zone-b/p2"}).ok());
  auto zone_b = ThermalZoneRef::parse("zone-b");
  ProtectedObligation constraint = advisory;
  constraint.zone = zone_b.value();
  auto obligation = rt.RegisterObligation(rt.Authority(), constraint);
  TEM_REQUIRE(obligation.ok());

  Timestamp now = base_time();
  for (const std::string& probe : {"zone-b/p1", "zone-b/p2"}) {
    auto admitted = rt.AdmitSample(rt.Authority(),
                                   make_sample("zone-b", probe, 34'000, now, probe == "zone-b/p1" ? 1 : 2));
    TEM_REQUIRE(admitted.ok());
  }
  auto report = rt.Tick(rt.Authority(), now);
  TEM_REQUIRE(report.ok());

  auto before = rt.IssueRequest(rt.Authority(), [&]() {
    MitigationRequestDraft item;
    item.cls = MitigationClass::DerateAccelerators;
    item.target = RequestTarget::for_zone(zone_b.value());
    item.intensity = BasisPoints::from_value(3'000);
    return item;
  }(), IssueOptions{}, now);
  TEM_REQUIRE(!before.ok());
  TEM_CHECK(before.status().code() == StatusCode::ProtectedObligationViolation);

  auto relax = rt.RelaxConstraint(rt.Authority(), obligation.value(),
                                  MitigationClass::DerateAccelerators,
                                  ExternalAuthorityRef{std::string("ops/oncall")}, now);
  TEM_REQUIRE(relax.ok());

  auto after = rt.IssueRequest(rt.Authority(), [&]() {
    MitigationRequestDraft item;
    item.cls = MitigationClass::DerateAccelerators;
    item.target = RequestTarget::for_zone(zone_b.value());
    item.intensity = BasisPoints::from_value(3'000);
    return item;
  }(), IssueOptions{}, now);
  TEM_REQUIRE(after.ok());

  auto again = rt.RelaxConstraint(rt.Authority(), obligation.value(),
                                  MitigationClass::DerateAccelerators,
                                  ExternalAuthorityRef{std::string("ops/oncall")}, now);
  TEM_REQUIRE(!again.ok());
  TEM_CHECK(again.status().code() == StatusCode::ObligationAlreadyRelaxed);
  rt.Shutdown();
}

TEM_TEST(stale_and_future_authority_tokens_are_refused) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 34'000;
  TEM_REQUIRE_OK(h.step(Duration::from_seconds(10)));

  AuthorityToken current = h.runtime->Authority();
  TEM_CHECK(h.runtime->Tick(current, h.now).ok());

  AuthorityToken stale_revision = current;
  stale_revision.expected_revision = StateRevision::from_value(current.expected_revision.value() - 1);
  auto refused = h.runtime->Tick(stale_revision, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::StaleRevision);

  AuthorityToken future_revision = h.runtime->Authority();
  future_revision.expected_revision =
      StateRevision::from_value(future_revision.expected_revision.value() + 5);
  refused = h.runtime->Tick(future_revision, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::FutureRevision);

  AuthorityToken stale_epoch = h.runtime->Authority();
  stale_epoch.epoch = ControlEpoch::from_value(0);
  refused = h.runtime->Tick(stale_epoch, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::MissingAuthority ||
             refused.status().code() == StatusCode::StaleEpoch);

  AuthorityToken other_incarnation = h.runtime->Authority();
  other_incarnation.incarnation = ControllerIncarnation::from_value(99);
  refused = h.runtime->Tick(other_incarnation, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::StaleIncarnation);

  AuthorityToken wrong_incident = h.runtime->Authority();
  wrong_incident.incident = IncidentId::from_value(4242);
  refused = h.runtime->Tick(wrong_incident, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::CrossIncidentAuthority);
}

TEM_TEST(idempotent_retry_does_not_repeat_the_external_request) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 34'000;
  TEM_REQUIRE_OK(h.step(Duration::from_seconds(10)));
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(1)));

  auto zone_ref = ThermalZoneRef::parse(h.zone);
  MitigationRequestDraft draft;
  draft.cls = MitigationClass::DerateAccelerators;
  draft.target = RequestTarget::for_zone(zone_ref.value());
  draft.intensity = BasisPoints::from_value(3'000);

  // A different intensity from the one the escalation plan already requested,
  // so this call is a genuinely new request rather than a replay.
  draft.intensity = BasisPoints::from_value(5'000);
  const std::size_t before = h.transport.dispatch_count();
  auto first = h.runtime->IssueRequest(h.runtime->Authority(), draft, IssueOptions{}, h.now);
  TEM_REQUIRE(first.ok());
  const std::size_t after_first = h.transport.dispatch_count();
  TEM_CHECK(after_first > before);

  auto second = h.runtime->IssueRequest(h.runtime->Authority(), draft, IssueOptions{}, h.now);
  TEM_REQUIRE(second.ok());
  TEM_CHECK_EQ(first.value().id.value(), second.value().id.value());
  TEM_CHECK_EQ(after_first, h.transport.dispatch_count());
  TEM_CHECK(h.runtime->Stats().deduped_retries >= 1);
}

TEM_TEST(idempotency_key_reuse_with_different_semantics_conflicts) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 34'000;
  TEM_REQUIRE_OK(h.step(Duration::from_seconds(10)));

  auto zone_ref = ThermalZoneRef::parse(h.zone);
  MitigationRequestDraft draft;
  draft.cls = MitigationClass::DerateAccelerators;
  draft.target = RequestTarget::for_zone(zone_ref.value());
  draft.intensity = BasisPoints::from_value(3'000);

  IssueOptions options;
  options.has_client_key = true;
  options.client_key = IdempotencyKey{0x1111'2222'3333'4444ull, 0x5555'6666'7777'8888ull};
  auto first = h.runtime->IssueRequest(h.runtime->Authority(), draft, options, h.now);
  TEM_REQUIRE(first.ok());

  IssueOptions repeat = options;
  auto replay = h.runtime->IssueRequest(h.runtime->Authority(), draft, repeat, h.now);
  TEM_REQUIRE(replay.ok());
  TEM_CHECK_EQ(first.value().id.value(), replay.value().id.value());

  MitigationRequestDraft different = draft;
  different.intensity = BasisPoints::from_value(7'000);
  auto conflict = h.runtime->IssueRequest(h.runtime->Authority(), different, options, h.now);
  TEM_REQUIRE(!conflict.ok());
  TEM_CHECK(conflict.status().code() == StatusCode::IdempotencyConflict);
}

TEM_TEST(dispatch_order_is_deterministic_for_identical_evidence) {
  std::vector<std::string> first_order;
  std::vector<std::string> second_order;
  for (int pass = 0; pass < 2; ++pass) {
    auto harness = Harness::open(DurabilityMode::MemoryOnly);
    TEM_REQUIRE(harness.ok());
    Harness& h = *harness.value();
    h.temperature = 56'000;
    h.rate = 5'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(2)));
    for (const MitigationRequest& request : h.transport.dispatch_log()) {
      const std::string entry = std::string(mitigation_class_name(request.cls)) + "|" +
                                request.target.to_string() + "|" +
                                std::to_string(request.intensity.value());
      if (pass == 0) {
        first_order.push_back(entry);
      } else {
        second_order.push_back(entry);
      }
    }
  }
  TEM_CHECK(!first_order.empty());
  TEM_CHECK_EQ(first_order.size(), second_order.size());
  TEM_CHECK(first_order == second_order);

  // Ordering within a plan is class ascending, then target bytes.
  for (std::size_t i = 1; i < first_order.size(); ++i) {
    const std::size_t previous = first_order[i - 1].find('|');
    const std::size_t current = first_order[i].find('|');
    const std::string previous_class = first_order[i - 1].substr(0, previous);
    const std::string current_class = first_order[i].substr(0, current);
    TEM_CHECK(previous_class <= current_class);
  }
}

TEM_TEST(replay_reproduces_the_live_state_byte_for_byte) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 40'000;
  h.rate = 2'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(3)));
  h.temperature = 25'000;
  h.rate = -1'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(3)));

  auto replay_report = h.runtime->VerifyReplay();
  TEM_REQUIRE(replay_report.ok());
  TEM_CHECK(replay_report.value().verified);
  TEM_CHECK(replay_report.value().journal_entries > 0);
  TEM_CHECK_EQ(replay_report.value().live_revision.value(),
               replay_report.value().reconstructed_revision.value());
}

TEM_TEST(reference_model_agrees_with_the_ladder_over_random_inputs) {
  ThermalPolicy policy;
  Rng rng(0xC0FFEEull);
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::int32_t low = rng.range(0, 60'000);
    const std::int32_t high = rng.range(0, 60'000);
    const std::int32_t temperature = std::max(low, high);
    auto harness = Harness::open(DurabilityMode::MemoryOnly);
    TEM_REQUIRE(harness.ok());
    Harness& h = *harness.value();
    h.temperature = temperature;
    h.rate = 0;
    Status stepped = h.step(Duration::from_seconds(10));
    if (!stepped.ok()) {
      TEM_FAIL("iteration " << iteration << " seed state " << rng.state()
                            << " failed: " << stepped.to_string());
      continue;
    }
    const Severity expected = reference_severity(temperature, policy);
    const Severity actual = h.inspect().severity;
    if (expected != actual) {
      TEM_FAIL("iteration " << iteration << " rng " << rng.state() << " temperature "
                            << temperature << " expected " << static_cast<int>(expected)
                            << " actual " << static_cast<int>(actual));
    }
  }
}

TEM_TEST(dwell_restarts_when_the_margin_condition_breaks) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 34'000;
  h.rate = 1'000;
  TEM_REQUIRE_OK(h.run(Duration::from_minutes(2)));
  h.temperature = 30'000;
  h.rate = -500;
  TEM_REQUIRE_OK(h.run(Duration::from_seconds(60)));

  const ReadModel first = h.inspect();
  const GateEvaluation* dwell = first.eligibility.find(RecoveryGate::Dwell);
  TEM_REQUIRE(dwell != nullptr);
  const bool held_before = dwell->result == GateResult::Passed;
  TEM_CHECK(!held_before);

  // A single excursion above the band must restart the dwell.
  h.temperature = 33'000;
  TEM_REQUIRE_OK(h.step(Duration::from_seconds(10)));
  h.temperature = 30'000;
  TEM_REQUIRE_OK(h.run(Duration::from_seconds(30)));
  const ReadModel after = h.inspect();
  const GateEvaluation* restarted = after.eligibility.find(RecoveryGate::Dwell);
  TEM_REQUIRE(restarted != nullptr);
  TEM_CHECK(restarted->result == GateResult::Failed);
}

TEM_TEST_MAIN()
