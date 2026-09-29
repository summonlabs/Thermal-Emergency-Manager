// Thermal Emergency Manager -- synthetic thermal scenario engine.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tem/refs.hpp"
#include "tem/runtime.hpp"
#include "tem/status.hpp"
#include "tem/transport.hpp"
#include "tem/units.hpp"

namespace summon::tem {

// Everything produced by this engine is SYNTHETIC. No chiller, CDU, CRAH, pump,
// valve, breaker, BMS, or DCIM system is contacted: the plant model below is a
// deterministic arithmetic model of a thermal excursion, and the transport is
// an in-process script. These scenarios exercise emergency semantics; they are
// not hardware validation.
struct ZoneSpec {
  ThermalZoneRef zone{};
  std::vector<ThermalProbeRef> probes{};
  PowerDomainRef power_domain{};
  std::vector<EquipmentRef> equipment{};
  MilliCelsius initial{MilliCelsius::from_value(22'000)};
  MilliCelsiusPerMinute ramp{};
  MilliCelsius ceiling{MilliCelsius::from_value(250'000)};
};

struct ObligationSpec {
  std::string ref{};
  ThermalZoneRef zone{};
  std::string description{};
  ProtectionClass protection{ProtectionClass::AdvisoryOptimization};
  std::uint8_t forbidden_classes{0};
  ExternalAuthorityRef authority{};
};

struct ScenarioConfig {
  std::vector<ZoneSpec> zones{};
  std::vector<ObligationSpec> obligations{};
  ThermalPolicy policy{};
  Duration sample_interval{Duration::from_seconds(5)};
  Duration tick_interval{Duration::from_seconds(5)};
  // Synthetic delay before the scenario harness confirms that an acknowledged
  // mitigation is actually in effect.
  Duration verification_delay{Duration::from_seconds(20)};
  Timestamp start{};
  std::uint32_t seed{1};
  SensorSourceRef source{};
};

enum class ScenarioId : std::uint8_t {
  NominalSteady = 1,
  RapidRise = 2,
  StaleSensors = 3,
  ContradictorySensors = 4,
  PartialAcknowledgement = 5,
  FailedDrain = 6,
  FailedPowerReduction = 7,
  CoolingUnavailable = 8,
  IsolationSuccess = 9,
  IsolationFailure = 10,
  RestartMidEmergency = 11,
  AuthorityRollover = 12,
  RecoveryWithHysteresis = 13,
  ObligationBlocked = 14,
  FullExcursion = 15,
};

[[nodiscard]] std::string_view scenario_name(ScenarioId id) noexcept;
[[nodiscard]] std::vector<ScenarioId> all_scenarios();
[[nodiscard]] Result<ScenarioId> scenario_from_name(std::string_view name);

struct ScenarioOutcome {
  ScenarioId id{ScenarioId::NominalSteady};
  std::string name{};
  bool ok{false};
  std::string failure_detail{};
  Severity peak_severity{Severity::Nominal};
  Severity final_severity{Severity::Nominal};
  Lifecycle final_lifecycle{Lifecycle::None};
  std::uint32_t restarts{0};
  std::uint32_t ticks{0};
  std::uint32_t samples{0};
  std::uint32_t dispatches{0};
  std::uint64_t deduped_retries{0};
  std::uint64_t requests_issued{0};
  std::uint64_t requests_verified{0};
  std::uint64_t requests_failed{0};
  std::uint64_t requests_abandoned{0};
  std::uint64_t requests_superseded{0};
  bool recovery_eligible{false};
  bool closed{false};
  std::vector<std::string> timeline{};
  ReadModel final_model{};
};

// Deterministic synthetic plant model.
class SyntheticPlant {
 public:
  explicit SyntheticPlant(std::vector<ZoneSpec> zones, Timestamp origin);

  // Sets the unmitigated ramp of the zone. Mitigation effects are always
  // computed from this base, so repeated application never compounds.
  void SetRamp(const ThermalZoneRef& zone, MilliCelsiusPerMinute ramp);
  // Applies an effective ramp without changing the base.
  void SetEffectiveRamp(const ThermalZoneRef& zone, MilliCelsiusPerMinute ramp);
  [[nodiscard]] MilliCelsiusPerMinute base_ramp(const ThermalZoneRef& zone) const;
  void SetInitial(const ThermalZoneRef& zone, MilliCelsius initial);
  void SetCeiling(const ThermalZoneRef& zone, MilliCelsius ceiling);
  void SetQuality(const ThermalProbeRef& probe, SampleQuality quality, SensorHealth health);
  void FreezeProbe(const ThermalProbeRef& probe);  // stops emitting; the stored sample ages out
  void SetOffset(const ThermalProbeRef& probe, MilliCelsius offset);
  void ResumeProbe(const ThermalProbeRef& probe);
  [[nodiscard]] bool frozen(const ThermalProbeRef& probe) const;

  // Produces the sample the modelled zone would report at the given instant.
  [[nodiscard]] Result<ThermalSample> Sample(const ThermalZoneRef& zone, const ThermalProbeRef& probe,
                                             Timestamp at, const SensorSourceRef& source,
                                             ObservationSequence sequence) const;

  [[nodiscard]] MilliCelsius temperature_at(const ThermalZoneRef& zone, Timestamp at) const;

  // The modelled probe state, exposed read-only so a scenario driver can walk
  // the facility it created.
  struct ProbeState {
    ThermalProbeRef probe{};
    SampleQuality quality{SampleQuality::Good};
    SensorHealth health{SensorHealth::Healthy};
    MilliCelsius offset{};
    bool frozen{false};
  };
  struct ZoneState {
    ZoneSpec spec{};
    MilliCelsiusPerMinute base_ramp{};
    std::vector<ProbeState> probes{};
  };

  [[nodiscard]] const std::vector<ZoneState>& zones() const noexcept { return zones_; }

 private:
  [[nodiscard]] ZoneState* find(const ThermalZoneRef& zone);
  [[nodiscard]] const ZoneState* find(const ThermalZoneRef& zone) const;
  [[nodiscard]] static ProbeState* find_probe(ZoneState& zone, const ThermalProbeRef& probe);
  [[nodiscard]] static const ProbeState* find_probe(const ZoneState& zone,
                                                    const ThermalProbeRef& probe);

  std::vector<ZoneState> zones_{};
  Timestamp origin_{};
};

// Runs a named synthetic scenario end to end. The factory lets a scenario close
// the runtime and reopen it, which is how restart and epoch-rollover scenarios
// are exercised without pretending that reopening in place is a restart.
class ScenarioRunner {
 public:
  // The factory receives the scenario's transport so that the runtime it
  // creates dispatches into the same scripted adjacent authority the scenario
  // configures.
  using RuntimeFactory = std::function<Result<std::unique_ptr<EmergencyRuntime>>(
      ControllerIncarnation, ControlEpoch, IMitigationTransport&)>;

  ScenarioRunner(RuntimeFactory factory, ScenarioConfig config);

  [[nodiscard]] Result<ScenarioOutcome> Run(ScenarioId id);

 private:
  RuntimeFactory factory_;
  ScenarioConfig config_;
};

}  // namespace summon::tem
