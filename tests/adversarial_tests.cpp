// Thermal Emergency Manager -- adversarial hardening tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "support.hpp"
#include "tem/plan.hpp"
#include "tem/store.hpp"

using namespace summon::tem;
using namespace temtest;

namespace {

// A transport that calls back into the runtime it was called from.
class ReentrantTransport final : public IMitigationTransport {
 public:
  void SetRuntime(EmergencyRuntime* runtime) { runtime_ = runtime; }
  void SetClock(Timestamp now) { now_ = now; }
  [[nodiscard]] DispatchResult Dispatch(const MitigationRequest&) override {
    DispatchResult result;
    result.outcome = DispatchOutcome::Accepted;
    if (runtime_ != nullptr) {
      auto reentered = runtime_->Tick(runtime_->Authority(), now_);
      observed_code_ = reentered.status().code();
    }
    return result;
  }
  [[nodiscard]] StatusCode observed_code() const { return observed_code_; }
  [[nodiscard]] std::string_view name() const noexcept override { return "reentrant"; }

 private:
  EmergencyRuntime* runtime_{nullptr};
  Timestamp now_{};
  StatusCode observed_code_{StatusCode::Ok};
};

// A transport that blocks until released, so another thread can change
// authority state while a dispatch is in flight.
class BlockingTransport final : public IMitigationTransport {
 public:
  [[nodiscard]] DispatchResult Dispatch(const MitigationRequest&) override {
    dispatched_.store(true);
    while (!released_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DispatchResult result;
    result.outcome = DispatchOutcome::Accepted;
    return result;
  }
  void WaitForDispatch() {
    while (!dispatched_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void Release() { released_.store(true); }
  [[nodiscard]] std::string_view name() const noexcept override { return "blocking"; }

 private:
  std::atomic<bool> dispatched_{false};
  std::atomic<bool> released_{false};
};

bool is_expected_race_refusal(StatusCode code) {
  switch (code) {
    case StatusCode::StaleRevision:
    case StatusCode::EvidenceOutOfOrder:
    case StatusCode::EvidenceDuplicate:
    case StatusCode::PreconditionFailed:
      return true;
    default:
      return false;
  }
}

}  // namespace

TEM_TEST(malformed_samples_are_refused_with_specific_codes) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  ThermalSample no_reading = make_sample("zone-a", "zone-a/p1", 0, base_time(), 1,
                                         SampleQuality::Unavailable);
  no_reading.has_temperature = true;  // inconsistent: unavailable but carrying a value
  auto refused = h.runtime->AdmitSample(h.runtime->Authority(), no_reading);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::InvalidArgument);

  ThermalSample missing_source = make_sample("zone-a", "zone-a/p1", 30'000, base_time(), 1);
  missing_source.source = SensorSourceRef{};
  refused = h.runtime->AdmitSample(h.runtime->Authority(), missing_source);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::EmptyField);

  ThermalSample unknown_probe = make_sample("zone-a", "zone-a/unknown", 30'000, base_time(), 1);
  refused = h.runtime->AdmitSample(h.runtime->Authority(), unknown_probe);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::EvidenceUnknownTarget);

  ThermalSample future = make_sample("zone-a", "zone-a/p1", 30'000,
                                     base_time().checked_add(Duration::from_minutes(5)).value_or(base_time()),
                                     1);
  // The reading claims to have been observed well after it was received.
  future.received_at = base_time();
  refused = h.runtime->AdmitSample(h.runtime->Authority(), future);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::EvidenceFuture);

  ThermalSample zero_sequence = make_sample("zone-a", "zone-a/p1", 30'000, base_time(), 0);
  refused = h.runtime->AdmitSample(h.runtime->Authority(), zero_sequence);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::ValueOutOfRange);
}

TEM_TEST(reordered_and_duplicate_evidence_is_refused) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  Timestamp now = base_time();
  auto first = h.runtime->AdmitSample(h.runtime->Authority(),
                                      make_sample("zone-a", "zone-a/p1", 30'000, now, 5));
  TEM_REQUIRE(first.ok());
  auto duplicate = h.runtime->AdmitSample(h.runtime->Authority(),
                                          make_sample("zone-a", "zone-a/p1", 30'000, now, 5));
  TEM_REQUIRE(!duplicate.ok());
  TEM_CHECK(duplicate.status().code() == StatusCode::EvidenceDuplicate);
  auto out_of_order = h.runtime->AdmitSample(h.runtime->Authority(),
                                             make_sample("zone-a", "zone-a/p1", 31'000, now, 4));
  TEM_REQUIRE(!out_of_order.ok());
  TEM_CHECK(out_of_order.status().code() == StatusCode::EvidenceOutOfOrder);

  // A clock that moves backwards is refused rather than silently accepted.
  Timestamp earlier = Timestamp::from_unix_nanos(now.unix_nanos() - Duration::from_minutes(1).nanos());
  auto backwards = h.runtime->Tick(h.runtime->Authority(), earlier);
  TEM_REQUIRE(!backwards.ok());
  TEM_CHECK(backwards.status().code() == StatusCode::PreconditionFailed);
}

TEM_TEST(duplicate_registration_and_unregistered_targets_are_refused) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  auto zone = ThermalZoneRef::parse("zone-a");
  auto again = h.runtime->RegisterZone(h.runtime->Authority(), zone.value());
  TEM_REQUIRE(!again.ok());
  TEM_CHECK(again.status().code() == StatusCode::DuplicateElement);

  auto probe = ThermalProbeRef::parse("zone-a/p1");
  auto probe_again = h.runtime->RegisterProbe(h.runtime->Authority(), zone.value(), probe.value());
  TEM_REQUIRE(!probe_again.ok());
  TEM_CHECK(probe_again.status().code() == StatusCode::DuplicateElement);

  auto missing_zone = ThermalZoneRef::parse("zone-missing");
  auto missing_probe = ThermalProbeRef::parse("zone-missing/p1");
  auto orphan = h.runtime->RegisterProbe(h.runtime->Authority(), missing_zone.value(),
                                         missing_probe.value());
  TEM_REQUIRE(!orphan.ok());
  TEM_CHECK(orphan.status().code() == StatusCode::EvidenceUnknownTarget);

  TargetBinding binding;
  binding.zone = missing_zone.value();
  auto binding_status = h.runtime->RegisterTargetBinding(h.runtime->Authority(), binding);
  TEM_REQUIRE(!binding_status.ok());
  TEM_CHECK(binding_status.status().code() == StatusCode::EvidenceUnknownTarget);
}

TEM_TEST(oversized_inputs_are_refused_before_allocation) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  ProtectedObligation obligation;
  obligation.ref = "obligation/big";
  obligation.protection = ProtectionClass::HardSafetyInterlock;
  obligation.authority = ExternalAuthorityRef{std::string("ops/safety")};
  obligation.forbidden_classes = mitigation_class_bit(MitigationClass::ReducePower);
  obligation.description = std::string(10'000, 'x');
  auto refused = h.runtime->RegisterObligation(h.runtime->Authority(), obligation);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::FieldTooLong);

  h.temperature = 40'000;
  TEM_REQUIRE(h.step(Duration::from_seconds(10)).ok());
  auto zone = ThermalZoneRef::parse("zone-a");
  MitigationRequestDraft draft;
  draft.cls = MitigationClass::DerateAccelerators;
  draft.target = RequestTarget::for_zone(zone.value());
  draft.intensity = BasisPoints::from_value(3'000);
  for (int i = 0; i < 200; ++i) {
    draft.evidence_refs.push_back(zone.value().token());
  }
  auto too_many = h.runtime->IssueRequest(h.runtime->Authority(), draft, IssueOptions{}, h.now);
  TEM_REQUIRE(!too_many.ok());
  TEM_CHECK(too_many.status().code() == StatusCode::TooManyElements);
}

TEM_TEST(request_target_must_match_the_class) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();
  h.temperature = 40'000;
  TEM_REQUIRE(h.step(Duration::from_seconds(10)).ok());

  auto domain = PowerDomainRef::parse("power/domain-a");
  MitigationRequestDraft draft;
  draft.cls = MitigationClass::DerateAccelerators;
  draft.target = RequestTarget::for_power_domain(domain.value());
  draft.intensity = BasisPoints::from_value(1'000);
  auto refused = h.runtime->IssueRequest(h.runtime->Authority(), draft, IssueOptions{}, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::RequestTargetMismatch);
}

TEM_TEST(a_reentrant_transport_is_refused_rather_than_deadlocking) {
  TempDir dir("reentrant");
  ReentrantTransport transport;
  RuntimeOptions options = durable_options(dir.path());
  options.transport = &transport;
  auto runtime = EmergencyRuntime::Open(options);
  TEM_REQUIRE(runtime.ok());
  transport.SetRuntime(runtime.value().get());
  transport.SetClock(base_time());
  TEM_REQUIRE(register_facility(*runtime.value(), "zone-a", {"zone-a/p1", "zone-a/p2"}).ok());

  Timestamp now = base_time();
  for (const std::string& probe : {"zone-a/p1", "zone-a/p2"}) {
    auto admitted = runtime.value()->AdmitSample(
        runtime.value()->Authority(),
        make_sample("zone-a", probe, 40'000, now, probe == "zone-a/p1" ? 1 : 2));
    TEM_REQUIRE(admitted.ok());
  }
  transport.SetClock(now);
  auto report = runtime.value()->Tick(runtime.value()->Authority(), now);
  TEM_REQUIRE(report.ok());
  TEM_CHECK(transport.observed_code() == StatusCode::ReentrancyRefused);
  runtime.value()->Shutdown();
}

TEM_TEST(a_stale_dispatch_answer_is_not_recorded_after_the_request_is_superseded) {
  TempDir dir("stale-answer");
  BlockingTransport transport;
  RuntimeOptions options = durable_options(dir.path());
  options.transport = &transport;
  auto runtime = EmergencyRuntime::Open(options);
  TEM_REQUIRE(runtime.ok());
  EmergencyRuntime& rt = *runtime.value();
  TEM_REQUIRE(register_facility(rt, "zone-a", {"zone-a/p1", "zone-a/p2"}).ok());

  Timestamp now = base_time();
  for (const std::string& probe : {"zone-a/p1", "zone-a/p2"}) {
    auto admitted = rt.AdmitSample(
        rt.Authority(), make_sample("zone-a", probe, 40'000, now, probe == "zone-a/p1" ? 1 : 2));
    TEM_REQUIRE(admitted.ok());
  }

  std::thread dispatcher([&rt, now]() {
    auto report = rt.Tick(rt.Authority(), now);
    (void)report;
  });
  transport.WaitForDispatch();

  // While the dispatch is in flight the request is still only planned, so the
  // operator path can abandon it. The late answer must then be dropped.
  const ReadModel model = rt.Inspect(now);
  TEM_REQUIRE(!model.requests.empty());
  const MitigationRequestId target = model.requests.front().request.id;
  auto abandoned = rt.AbandonRequest(rt.Authority(), target, now);
  TEM_REQUIRE(abandoned.ok());
  transport.Release();
  dispatcher.join();

  const ReadModel after = rt.Inspect(now);
  for (const MitigationRecord& record : after.requests) {
    if (record.request.id == target) {
      TEM_CHECK(record.state == MitigationState::Abandoned);
    }
  }
  rt.Shutdown();
}

TEM_TEST(concurrent_mutation_from_many_threads_is_serialised_and_consistent) {
  TempDir dir("concurrent");
  auto runtime = EmergencyRuntime::Open(durable_options(dir.path()));
  TEM_REQUIRE(runtime.ok());
  EmergencyRuntime& rt = *runtime.value();
  TEM_REQUIRE(register_facility(rt, "zone-a", {"zone-a/p1", "zone-a/p2"}).ok());

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&rt, &failures, worker]() {
      Timestamp now = base_time();
      std::uint64_t sequence = static_cast<std::uint64_t>(worker) * 1'000 + 1;
      for (int step = 0; step < 20; ++step) {
        now = now.checked_add(Duration::from_seconds(10)).value_or(now);
        for (const std::string& probe : {"zone-a/p1", "zone-a/p2"}) {
          auto admitted = rt.AdmitSample(
              rt.Authority(),
              make_sample("zone-a", probe, 30'000 + (step % 3) * 1'000, now, sequence++));
          // Racing threads legitimately produce deterministic refusals: a
          // stale token, an out-of-order sample, or an instant that another
          // thread has already passed. Anything else is a real defect.
          if (!admitted.ok() && !is_expected_race_refusal(admitted.status().code())) {
            ++failures;
          }
        }
        auto report = rt.Tick(rt.Authority(), now);
        if (!report.ok() && !is_expected_race_refusal(report.status().code())) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  TEM_CHECK_EQ(0, failures.load());
  auto replayed = rt.VerifyReplay();
  TEM_REQUIRE(replayed.ok());
  TEM_CHECK(replayed.value().verified);
  const ReadModel model = rt.Inspect(base_time());
  TEM_CHECK(model.zones.size() == 1);
  rt.Shutdown();
}

TEM_TEST(shutdown_is_idempotent_and_closes_the_authority) {
  TempDir dir("shutdown");
  auto runtime = EmergencyRuntime::Open(durable_options(dir.path()));
  TEM_REQUIRE(runtime.ok());
  EmergencyRuntime& rt = *runtime.value();
  TEM_REQUIRE(register_facility(rt, "zone-a", {"zone-a/p1", "zone-a/p2"}).ok());
  rt.Shutdown();
  TEM_CHECK(rt.closed());
  rt.Shutdown();
  auto after = rt.Tick(rt.Authority(), base_time());
  TEM_REQUIRE(!after.ok());
  TEM_CHECK(after.status().code() == StatusCode::RuntimeClosed);

  // Repeated open/close of the same store is safe.
  for (int i = 0; i < 3; ++i) {
    auto reopened = EmergencyRuntime::Open(
        durable_options(dir.path(), ControllerIncarnation::from_value(static_cast<std::uint64_t>(i + 2)),
                        ControlEpoch::from_value(static_cast<std::uint64_t>(i + 2))));
    TEM_REQUIRE(reopened.ok());
    auto replayed = reopened.value()->VerifyReplay();
    TEM_REQUIRE(replayed.ok());
    reopened.value()->Shutdown();
  }
}

TEM_TEST(recovery_cannot_be_started_without_preconditions) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  h.temperature = 45'000;
  h.rate = 3'000;
  TEM_REQUIRE(h.run(Duration::from_minutes(2)).ok());
  auto refused = h.runtime->BeginRecovery(h.runtime->Authority(),
                                          OperatorRef{std::string("ops/oncall")}, h.now);
  TEM_REQUIRE(!refused.ok());
  TEM_CHECK(refused.status().code() == StatusCode::IllegalTransition);

  auto closure = h.runtime->CloseIncident(h.runtime->Authority(),
                                          OperatorRef{std::string("ops/oncall")},
                                          Disposition::Recovered, h.now);
  TEM_REQUIRE(!closure.ok());
  TEM_CHECK(closure.status().code() == StatusCode::GateNotSatisfied);
}

TEM_TEST(severity_estimate_never_uses_missing_evidence_as_zero) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  // One probe never reports at all; the other reports a hot reading.
  auto admitted = h.runtime->AdmitSample(
      h.runtime->Authority(), make_sample("zone-a", "zone-a/p1", 41'000, base_time(), 1));
  TEM_REQUIRE(admitted.ok());
  auto report = h.runtime->Tick(h.runtime->Authority(), base_time());
  TEM_REQUIRE(report.ok());

  const ReadModel model = h.runtime->Inspect(base_time());
  TEM_REQUIRE(severity_ordinal(model.severity) >= severity_ordinal(Severity::Critical));
  TEM_CHECK(!model.evidence.all_fresh);
  bool absent_seen = false;
  for (const ZoneAssessment& zone : model.evidence.zones) {
    if (zone.cls == FreshnessClass::Absent || zone.fresh_probes < zone.total_probes) {
      absent_seen = true;
    }
  }
  TEM_CHECK(absent_seen);
  TEM_CHECK(!model.eligibility.eligible);
}

TEM_TEST(failed_requests_are_reissued_only_up_to_the_attempt_limit) {
  auto harness = Harness::open(DurabilityMode::MemoryOnly);
  TEM_REQUIRE(harness.ok());
  Harness& h = *harness.value();

  ScriptedTransport::Rule refuse;
  refuse.outcome = DispatchOutcome::Error;
  refuse.code = 3;
  refuse.detail = "unavailable";
  h.transport.SetDefaultRule(refuse);

  h.temperature = 40'000;
  h.rate = 1'000;
  TEM_REQUIRE(h.run(Duration::from_minutes(6)).ok());
  const ReadModel model = h.inspect();
  std::uint32_t attempts = 0;
  for (const MitigationRecord& record : model.requests) {
    if (record.request.cls == MitigationClass::DerateAccelerators) {
      ++attempts;
    }
  }
  TEM_CHECK(attempts <= kMaxRequestAttempts);
  TEM_CHECK(attempts >= 1);
  TEM_CHECK(!model.eligibility.eligible);
}

TEM_TEST_MAIN()
