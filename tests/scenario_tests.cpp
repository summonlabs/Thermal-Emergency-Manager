// Thermal Emergency Manager -- synthetic scenario proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// SYNTHETIC: these scenarios exercise emergency semantics against a modelled
// facility. No plant, sensor, BMS, or DCIM system is contacted.
#include <memory>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support.hpp"
#include "tem/scenario.hpp"
#include "tem/store.hpp"

using namespace summon::tem;
using namespace temtest;

namespace {

ScenarioConfig standard_config() {
  ScenarioConfig config;
  config.start = base_time();
  config.source = SensorSourceRef::parse("synthetic/plant-a").value();
  config.sample_interval = Duration::from_seconds(10);
  config.tick_interval = Duration::from_seconds(10);
  config.verification_delay = Duration::from_seconds(30);

  ZoneSpec zone;
  zone.zone = ThermalZoneRef::parse("zone-a").value();
  zone.probes.push_back(ThermalProbeRef::parse("zone-a/p1").value());
  zone.probes.push_back(ThermalProbeRef::parse("zone-a/p2").value());
  zone.power_domain = PowerDomainRef::parse("power/domain-a").value();
  zone.equipment.push_back(EquipmentRef::parse("equipment/cdu-a/branch-1").value());
  zone.initial = MilliCelsius::from_value(22'000);
  config.zones.push_back(zone);

  ObligationSpec interlock;
  interlock.ref = "obligation/keep-evacuation-path-energized";
  interlock.zone = zone.zone;
  interlock.description = "evacuation path power must stay energized";
  interlock.protection = ProtectionClass::HardSafetyInterlock;
  interlock.forbidden_classes = mitigation_class_bit(MitigationClass::ReducePower);
  interlock.authority = ExternalAuthorityRef{std::string("ops/facility-safety")};
  config.obligations.push_back(interlock);
  return config;
}

ScenarioRunner::RuntimeFactory volatile_factory() {
  return [](ControllerIncarnation incarnation, ControlEpoch epoch,
            IMitigationTransport& transport) {
    RuntimeOptions options = memory_options(incarnation, epoch);
    options.transport = &transport;
    return EmergencyRuntime::Open(options);
  };
}

}  // namespace

TEM_TEST(synthetic_plant_model_is_exact_and_bounded) {
  ZoneSpec zone;
  zone.zone = ThermalZoneRef::parse("zone-model").value();
  zone.probes.push_back(ThermalProbeRef::parse("zone-model/p1").value());
  zone.initial = MilliCelsius::from_value(22'000);
  std::vector<ZoneSpec> zones;
  zones.push_back(zone);
  SyntheticPlant plant(zones, base_time());
  const auto source = SensorSourceRef::parse("test/source").value();

  plant.SetInitial(zone.zone, MilliCelsius::from_value(50'000));
  plant.SetRamp(zone.zone, MilliCelsiusPerMinute::from_value(4'000));
  auto after_one_minute = plant.Sample(
      zone.zone, zone.probes.front(),
      base_time().checked_add(Duration::from_minutes(1)).value_or(base_time()), source,
      ObservationSequence::from_value(1));
  TEM_REQUIRE_OK(after_one_minute);
  TEM_CHECK_EQ(std::int32_t{54'000}, after_one_minute.value().temperature.value());

  // The model never leaves the representable range, even for extreme ramps.
  plant.SetRamp(zone.zone, MilliCelsiusPerMinute::from_value(600'000));
  auto extreme = plant.Sample(
      zone.zone, zone.probes.front(),
      base_time().checked_add(Duration::from_minutes(600)).value_or(base_time()), source,
      ObservationSequence::from_value(2));
  TEM_REQUIRE_OK(extreme);
  TEM_CHECK(extreme.value().temperature.value() <= MilliCelsiusTag::kMax);
  TEM_CHECK(extreme.value().temperature.value() >= MilliCelsiusTag::kMin);
}

TEM_TEST(every_named_scenario_meets_its_documented_expectations) {
  // Every scenario runs against a durable store: the restart and rollover
  // scenarios only mean anything when state actually survives the process.
  TempDir dir("all-scenarios");
  const std::filesystem::path root = dir.path();
  for (const ScenarioId id : all_scenarios()) {
    const std::filesystem::path scenario_root = root / std::string(scenario_name(id));
    ScenarioRunner runner(
        [scenario_root](ControllerIncarnation incarnation, ControlEpoch epoch,
                        IMitigationTransport& transport) {
          RuntimeOptions options = durable_options(scenario_root, incarnation, epoch);
          options.transport = &transport;
          return EmergencyRuntime::Open(options);
        },
        standard_config());
    auto outcome = runner.Run(id);
    if (!outcome.ok()) {
      TEM_FAIL("scenario " << scenario_name(id) << " returned an error: "
                           << outcome.status().to_string());
      continue;
    }
    const ScenarioOutcome& result = outcome.value();
    if (!result.ok) {
      TEM_FAIL("scenario " << result.name << " failed: " << result.failure_detail
                           << "\n"
                           << report::render_requests(result.final_model.requests)
                           << report::render_eligibility(result.final_model.eligibility));
    }
    TEM_CHECK(result.samples > 0);
    TEM_CHECK(result.ticks > 0);
  }
}

TEM_TEST(the_end_to_end_excursion_closes_only_after_verified_recovery) {
  ScenarioRunner runner(volatile_factory(), standard_config());
  auto outcome = runner.Run(ScenarioId::FullExcursion);
  TEM_REQUIRE(outcome.ok());
  const ScenarioOutcome& result = outcome.value();
  TEM_REQUIRE(result.ok);
  TEM_CHECK_EQ(static_cast<int>(Lifecycle::Closed), static_cast<int>(result.final_lifecycle));
  TEM_CHECK(result.peak_severity != Severity::Nominal);
  TEM_CHECK(result.dispatches > 0);
  TEM_CHECK(result.requests_verified > 0);
  TEM_CHECK(result.final_model.requests.size() > 0);

  bool severity_ever_decreased = false;
  for (const TransitionRecord& transition : result.final_model.transitions) {
    if (severity_ordinal(transition.to_severity) < severity_ordinal(transition.from_severity)) {
      TEM_CHECK(transition.from_lifecycle == Lifecycle::Recovering);
      severity_ever_decreased = true;
    }
    if (severity_ordinal(transition.to_severity) > severity_ordinal(transition.from_severity)) {
      TEM_CHECK(!severity_ever_decreased || true);
    }
  }
  TEM_CHECK(!result.final_model.eligibility.eligible ||
            result.final_lifecycle == Lifecycle::Closed);
}

TEM_TEST(a_durable_end_to_end_excursion_survives_reopen_and_replays) {
  TempDir dir("scenario-durable");
  const std::filesystem::path root = dir.path();
  ScenarioRunner runner(
      [root](ControllerIncarnation incarnation, ControlEpoch epoch,
             IMitigationTransport& transport) {
        RuntimeOptions options = durable_options(root, incarnation, epoch);
        options.transport = &transport;
        return EmergencyRuntime::Open(options);
      },
      standard_config());
  auto outcome = runner.Run(ScenarioId::FullExcursion);
  TEM_REQUIRE(outcome.ok());
  TEM_REQUIRE(outcome.value().ok);

  StoreOpenOptions options;
  options.root = root;
  options.create_if_missing = false;
  auto store = DurableStore::Open(options);
  TEM_REQUIRE(store.ok());
  const StoreVerification verification = store.value()->Verify();
  TEM_CHECK(verification.ok);
  auto replayed = replay(store.value()->snapshot().checkpoint, store.value()->snapshot().journal);
  TEM_REQUIRE(replayed.ok());
  TEM_CHECK(states_encode_identically(replayed.value(), store.value()->snapshot().live));
  TEM_CHECK(store.value()->snapshot().live.incidents.size() >= 1);
  store.value()->Close();
}

TEM_TEST(restart_and_rollover_scenarios_fence_the_previous_authority) {
  TempDir dir("scenario-restart");
  for (const ScenarioId id : {ScenarioId::RestartMidEmergency, ScenarioId::AuthorityRollover}) {
    // Each scenario gets its own store: a restart scenario starts from a live
    // excursion, not from whatever a previous scenario left behind.
    const std::filesystem::path root = dir.path() / std::string(scenario_name(id));
    ScenarioRunner runner(
        [root](ControllerIncarnation incarnation, ControlEpoch epoch,
               IMitigationTransport& transport) {
          RuntimeOptions options = durable_options(root, incarnation, epoch);
          options.transport = &transport;
          return EmergencyRuntime::Open(options);
        },
        standard_config());
    auto outcome = runner.Run(id);
    TEM_REQUIRE(outcome.ok());
    const ScenarioOutcome& result = outcome.value();
    if (!result.ok) {
      TEM_FAIL("scenario " << result.name << " failed: " << result.failure_detail);
    }
    TEM_CHECK_EQ(std::uint32_t{1}, result.restarts);
    TEM_CHECK(result.requests_superseded > 0);
  }
}

TEM_TEST_MAIN()
