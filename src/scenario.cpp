// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/scenario.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "tem/plan.hpp"

namespace summon::tem {

std::string_view scenario_name(ScenarioId id) noexcept {
  switch (id) {
    case ScenarioId::NominalSteady: return "nominal-steady";
    case ScenarioId::RapidRise: return "rapid-rise";
    case ScenarioId::StaleSensors: return "stale-sensors";
    case ScenarioId::ContradictorySensors: return "contradictory-sensors";
    case ScenarioId::PartialAcknowledgement: return "partial-acknowledgement";
    case ScenarioId::FailedDrain: return "failed-drain";
    case ScenarioId::FailedPowerReduction: return "failed-power-reduction";
    case ScenarioId::CoolingUnavailable: return "cooling-unavailable";
    case ScenarioId::IsolationSuccess: return "isolation-success";
    case ScenarioId::IsolationFailure: return "isolation-failure";
    case ScenarioId::RestartMidEmergency: return "restart-mid-emergency";
    case ScenarioId::AuthorityRollover: return "authority-rollover";
    case ScenarioId::RecoveryWithHysteresis: return "recovery-with-hysteresis";
    case ScenarioId::ObligationBlocked: return "obligation-blocked";
    case ScenarioId::FullExcursion: return "full-excursion";
  }
  return "unknown";
}

std::vector<ScenarioId> all_scenarios() {
  return {ScenarioId::NominalSteady,      ScenarioId::RapidRise,
          ScenarioId::StaleSensors,       ScenarioId::ContradictorySensors,
          ScenarioId::PartialAcknowledgement, ScenarioId::FailedDrain,
          ScenarioId::FailedPowerReduction, ScenarioId::CoolingUnavailable,
          ScenarioId::IsolationSuccess,   ScenarioId::IsolationFailure,
          ScenarioId::RestartMidEmergency, ScenarioId::AuthorityRollover,
          ScenarioId::RecoveryWithHysteresis, ScenarioId::ObligationBlocked,
          ScenarioId::FullExcursion};
}

Result<ScenarioId> scenario_from_name(std::string_view name) {
  for (const ScenarioId id : all_scenarios()) {
    if (scenario_name(id) == name) {
      return id;
    }
  }
  return Status::error(StatusCode::InvalidArgument, "unknown synthetic scenario name")
      .with_context(std::string(name));
}

// ---------------------------------------------------------------------------
// Synthetic plant
// ---------------------------------------------------------------------------

SyntheticPlant::SyntheticPlant(std::vector<ZoneSpec> zones, Timestamp origin) : origin_(origin) {
  for (ZoneSpec& spec : zones) {
    ZoneState state;
    state.spec = std::move(spec);
    state.base_ramp = state.spec.ramp;
    for (const ThermalProbeRef& probe : state.spec.probes) {
      ProbeState probe_state;
      probe_state.probe = probe;
      state.probes.push_back(probe_state);
    }
    zones_.push_back(std::move(state));
  }
}

SyntheticPlant::ZoneState* SyntheticPlant::find(const ThermalZoneRef& zone) {
  for (ZoneState& state : zones_) {
    if (state.spec.zone == zone) {
      return &state;
    }
  }
  return nullptr;
}

const SyntheticPlant::ZoneState* SyntheticPlant::find(const ThermalZoneRef& zone) const {
  for (const ZoneState& state : zones_) {
    if (state.spec.zone == zone) {
      return &state;
    }
  }
  return nullptr;
}

SyntheticPlant::ProbeState* SyntheticPlant::find_probe(ZoneState& zone, const ThermalProbeRef& probe) {
  for (ProbeState& state : zone.probes) {
    if (state.probe == probe) {
      return &state;
    }
  }
  return nullptr;
}

const SyntheticPlant::ProbeState* SyntheticPlant::find_probe(const ZoneState& zone,
                                                             const ThermalProbeRef& probe) {
  for (const ProbeState& state : zone.probes) {
    if (state.probe == probe) {
      return &state;
    }
  }
  return nullptr;
}

void SyntheticPlant::SetRamp(const ThermalZoneRef& zone, MilliCelsiusPerMinute ramp) {
  ZoneState* state = find(zone);
  if (state != nullptr) {
    state->base_ramp = ramp;
    state->spec.ramp = ramp;
  }
}

void SyntheticPlant::SetEffectiveRamp(const ThermalZoneRef& zone, MilliCelsiusPerMinute ramp) {
  ZoneState* state = find(zone);
  if (state != nullptr) {
    state->spec.ramp = ramp;
  }
}

MilliCelsiusPerMinute SyntheticPlant::base_ramp(const ThermalZoneRef& zone) const {
  const ZoneState* state = find(zone);
  return state != nullptr ? state->base_ramp : MilliCelsiusPerMinute{};
}

void SyntheticPlant::SetInitial(const ThermalZoneRef& zone, MilliCelsius initial) {
  ZoneState* state = find(zone);
  if (state != nullptr) {
    state->spec.initial = initial;
  }
}

void SyntheticPlant::SetCeiling(const ThermalZoneRef& zone, MilliCelsius ceiling) {
  ZoneState* state = find(zone);
  if (state != nullptr) {
    state->spec.ceiling = ceiling;
  }
}

void SyntheticPlant::SetQuality(const ThermalProbeRef& probe, SampleQuality quality,
                                SensorHealth health) {
  for (ZoneState& zone : zones_) {
    ProbeState* state = find_probe(zone, probe);
    if (state != nullptr) {
      state->quality = quality;
      state->health = health;
      return;
    }
  }
}

void SyntheticPlant::FreezeProbe(const ThermalProbeRef& probe) {
  for (ZoneState& zone : zones_) {
    ProbeState* state = find_probe(zone, probe);
    if (state != nullptr) {
      state->frozen = true;
      return;
    }
  }
}

void SyntheticPlant::ResumeProbe(const ThermalProbeRef& probe) {
  for (ZoneState& zone : zones_) {
    ProbeState* state = find_probe(zone, probe);
    if (state != nullptr) {
      state->frozen = false;
      return;
    }
  }
}

bool SyntheticPlant::frozen(const ThermalProbeRef& probe) const {
  for (const ZoneState& zone : zones_) {
    const ProbeState* state = find_probe(zone, probe);
    if (state != nullptr) {
      return state->frozen;
    }
  }
  return false;
}

void SyntheticPlant::SetOffset(const ThermalProbeRef& probe, MilliCelsius offset) {
  for (ZoneState& zone : zones_) {
    ProbeState* state = find_probe(zone, probe);
    if (state != nullptr) {
      state->offset = offset;
      return;
    }
  }
}

MilliCelsius SyntheticPlant::temperature_at(const ThermalZoneRef& zone, Timestamp at) const {
  const ZoneState* state = find(zone);
  if (state == nullptr) {
    return MilliCelsius{};
  }
  const std::int64_t elapsed_nanos = at.unix_nanos() - origin_.unix_nanos();
  const std::int64_t minutes = elapsed_nanos / 60'000'000'000ll;
  const std::int64_t remainder = elapsed_nanos % 60'000'000'000ll;
  std::int64_t delta = state->spec.ramp.value() * minutes;
  delta += (state->spec.ramp.value() * remainder) / 60'000'000'000ll;
  std::int64_t value = state->spec.initial.value() + delta;
  const std::int64_t ceiling = state->spec.ceiling.value();
  constexpr std::int64_t kFloor = -50'000;  // -50.000 C: the modelled plant never goes below this
  value = std::min<std::int64_t>(value, ceiling);
  value = std::max<std::int64_t>(value, kFloor);
  const auto bounded = MilliCelsius::try_make(static_cast<std::int32_t>(value));
  return bounded.has_value() ? *bounded : state->spec.initial;
}

Result<ThermalSample> SyntheticPlant::Sample(const ThermalZoneRef& zone,
                                             const ThermalProbeRef& probe, Timestamp at,
                                             const SensorSourceRef& source,
                                             ObservationSequence sequence) const {
  const ZoneState* zone_state = find(zone);
  if (zone_state == nullptr) {
    return Status::error(StatusCode::EvidenceUnknownTarget, "synthetic plant has no such zone");
  }
  const ProbeState* probe_state = find_probe(*zone_state, probe);
  if (probe_state == nullptr) {
    return Status::error(StatusCode::EvidenceUnknownTarget, "synthetic plant has no such probe");
  }
  if (probe_state->frozen) {
    return Status::error(StatusCode::EvidenceUnavailable,
                         "the synthetic probe is frozen and emits nothing");
  }
  ThermalSample sample;
  sample.zone = zone;
  sample.probe = probe;
  sample.source = source;
  sample.quality = probe_state->quality;
  sample.health = probe_state->health;
  sample.origin = SampleOrigin::Live;
  sample.observed_at = at;
  sample.received_at = at;
  sample.sequence = sequence;
  const bool carries = sample.quality == SampleQuality::Good || sample.quality == SampleQuality::Suspect;
  if (carries) {
    const auto base = temperature_at(zone, at);
    const auto shifted = base.checked_add(probe_state->offset);
    sample.has_temperature = true;
    sample.temperature = shifted.has_value() ? *shifted : base;
    sample.has_rate = true;
    sample.rate = zone_state->spec.ramp;
  } else {
    sample.has_temperature = false;
    sample.has_rate = false;
  }
  return sample;
}

// ---------------------------------------------------------------------------
// Scenario runner
// ---------------------------------------------------------------------------

namespace {

std::uint64_t count_requests(const ReadModel& model, MitigationClass cls, MitigationState state) {
  std::uint64_t count = 0;
  for (const MitigationRecord& record : model.requests) {
    if (record.request.cls == cls && record.state == state) {
      ++count;
    }
  }
  return count;
}

std::uint64_t count_requests(const ReadModel& model, MitigationState state) {
  std::uint64_t count = 0;
  for (const MitigationRecord& record : model.requests) {
    if (record.state == state) {
      ++count;
    }
  }
  return count;
}

// A scenario driving session. Every plant interaction is synthetic.
class Session {
 public:
  // The start instant defaults to the scenario start. A session that resumes
  // after a controller restart must start at the instant the previous session
  // stopped, because the restored store refuses any input that predates its
  // last accepted one.
  Session(EmergencyRuntime& runtime, SyntheticPlant& plant, const ScenarioConfig& config,
          ScriptedTransport& transport, ScenarioOutcome& outcome, Timestamp start)
      : runtime_(runtime),
        plant_(plant),
        config_(config),
        transport_(transport),
        outcome_(outcome),
        now_(start) {}

  Session(EmergencyRuntime& runtime, SyntheticPlant& plant, const ScenarioConfig& config,
          ScriptedTransport& transport, ScenarioOutcome& outcome)
      : Session(runtime, plant, config, transport, outcome, config.start) {}

  [[nodiscard]] Timestamp now() const { return now_; }
  [[nodiscard]] SyntheticPlant& plant() { return plant_; }
  [[nodiscard]] ScriptedTransport& transport() { return transport_; }
  [[nodiscard]] ScenarioOutcome& outcome() { return outcome_; }
  [[nodiscard]] EmergencyRuntime& runtime() { return runtime_; }

  void note(std::string text) {
    outcome_.timeline.push_back(format_timestamp(now_) + " " + std::move(text));
  }

  // Registers the modelled facility. Idempotent: a runtime that was reopened
  // over a durable store already holds the facility, so only what is missing is
  // registered.
  Status RegisterFacility() {
    const ReadModel existing = runtime_.Inspect(now_);
    for (const SyntheticPlant::ZoneState& zone : plant_.zones()) {
      const ZoneSlot* known_zone = nullptr;
      for (const ZoneSlot& slot : existing.zones) {
        if (slot.zone == zone.spec.zone) {
          known_zone = &slot;
          break;
        }
      }
      if (known_zone == nullptr) {
        auto zone_status = runtime_.RegisterZone(runtime_.Authority(), zone.spec.zone);
        if (!zone_status.ok()) {
          return zone_status.status();
        }
      }
      for (const SyntheticPlant::ProbeState& probe : zone.probes) {
        bool known = false;
        if (known_zone != nullptr) {
          for (const ProbeSlot& slot : known_zone->probes) {
            if (slot.probe == probe.probe) {
              known = true;
              break;
            }
          }
        }
        if (known) {
          continue;
        }
        auto probe_status =
            runtime_.RegisterProbe(runtime_.Authority(), zone.spec.zone, probe.probe);
        if (!probe_status.ok()) {
          return probe_status.status();
        }
      }
      if (known_zone == nullptr &&
          (zone.spec.power_domain.valid() || !zone.spec.equipment.empty())) {
        TargetBinding binding;
        binding.zone = zone.spec.zone;
        binding.power_domain = zone.spec.power_domain;
        binding.equipment = zone.spec.equipment;
        auto binding_status = runtime_.RegisterTargetBinding(runtime_.Authority(), binding);
        if (!binding_status.ok()) {
          return binding_status.status();
        }
      }
    }
    for (const ObligationSpec& spec : config_.obligations) {
      bool known = false;
      for (const ProtectedObligation& obligation : existing.obligations) {
        if (obligation.ref == spec.ref) {
          obligation_ids_[spec.ref] = obligation.id;
          known = true;
          break;
        }
      }
      if (known) {
        continue;
      }
      ProtectedObligation obligation;
      obligation.ref = spec.ref;
      obligation.zone = spec.zone;
      obligation.description = spec.description;
      obligation.protection = spec.protection;
      obligation.forbidden_classes = spec.forbidden_classes;
      obligation.authority = spec.authority;
      auto registered = runtime_.RegisterObligation(runtime_.Authority(), obligation);
      if (!registered.ok()) {
        return registered.status();
      }
      obligation_ids_[spec.ref] = registered.value();
    }
    // The authority that owns each obligation reports its live status; an
    // unreported obligation would keep the interlock gate unknown, which is the
    // safe default but not a realistic running facility.
    // A store that already holds observations continues the producer stream
    // rather than restarting it: a replayed sequence number would be refused as
    // out of order, and rightly so.
    for (const ZoneSlot& slot : runtime_.Inspect(now_).zones) {
      for (const ProbeSlot& probe : slot.probes) {
        if (probe.present && probe.last_sequence.value() >= sequence_) {
          sequence_ = probe.last_sequence.value() + 1;
        }
      }
    }
    Status reported = RefreshObligationStatus();
    if (!reported.ok()) {
      return reported;
    }
    return Status::success();
  }

  Status ReportObligation(const std::string& ref, ObligationStatus status) {
    const auto it = obligation_ids_.find(ref);
    if (it == obligation_ids_.end()) {
      return Status::error(StatusCode::ObligationUnknown, "scenario obligation is not registered");
    }
    auto reported = runtime_.ReportObligationStatus(
        runtime_.Authority(), it->second, status, config_.obligations.empty()
                                                    ? ExternalAuthorityRef{}
                                                    : ExternalAuthorityRef{std::string("ops/facility")},
        now_);
    return reported.status();
  }

  Status AdmitAll() {
    for (const SyntheticPlant::ZoneState& zone : plant_.zones()) {
      for (const SyntheticPlant::ProbeState& probe : zone.probes) {
        if (plant_.frozen(probe.probe)) {
          continue;
        }
        auto sample = plant_.Sample(zone.spec.zone, probe.probe, now_, config_.source,
                                    ObservationSequence::from_value(sequence_++));
        if (!sample.ok()) {
          continue;
        }
        auto admitted = runtime_.AdmitSample(runtime_.Authority(), sample.value());
        if (!admitted.ok()) {
          return admitted.status();
        }
        ++outcome_.samples;
      }
    }
    return Status::success();
  }

  Status Tick() {
    // Operator authority relaxes advisory optimisation constraints that block a
    // required mitigation. Hard interlocks and regulatory obligations are never
    // touched here: the runtime refuses those, and the scenario records nothing.
    Status relaxed = RelaxBlockingAdvisoryConstraints();
    if (!relaxed.ok()) {
      return relaxed;
    }
    auto report = runtime_.Tick(runtime_.Authority(), now_);
    if (!report.ok()) {
      return report.status();
    }
    ++outcome_.ticks;
    ApplyMitigationEffects();
    NoteSeverityChange();
    Status refreshed = RefreshObligationStatus();
    if (!refreshed.ok()) {
      return refreshed;
    }
    return VerifySettled();
  }

  // The owning authority reaffirms obligation status before it ages out: a
  // status that is no longer current is treated as unknown, which blocks
  // recovery by design.
  Status RefreshObligationStatus() {
    if (obligation_ids_.empty()) {
      return Status::success();
    }
    if (last_obligation_report_.unix_nanos() != 0 &&
        now_.unix_nanos() - last_obligation_report_.unix_nanos() <
            Duration::from_minutes(5).nanos()) {
      return Status::success();
    }
    for (const auto& entry : obligation_ids_) {
      auto reported = runtime_.ReportObligationStatus(
          runtime_.Authority(), entry.second, ObligationStatus::Satisfied,
          ExternalAuthorityRef{std::string("ops/facility-safety")}, now_);
      if (!reported.ok()) {
        return reported.status();
      }
    }
    last_obligation_report_ = now_;
    return Status::success();
  }

  // Synthetic stand-in for an adjacent authority confirming the effect. The
  // confirmation reference is an opaque handle; the runtime does not interpret
  // it and never treats an acknowledgement as the effect.
  Status VerifySettled() {
    const ReadModel model = runtime_.Inspect(now_);
    for (const MitigationRecord& record : model.requests) {
      bool needs_confirmation = false;
      if (record.state == MitigationState::Acknowledged ||
          record.state == MitigationState::Observed) {
        needs_confirmation =
            now_.unix_nanos() - record.state_at.unix_nanos() >= config_.verification_delay.nanos();
      } else if (record.state == MitigationState::Verified && record.has_verified_at) {
        // Confirmation evidence ages out on purpose; the authority reaffirms it.
        needs_confirmation =
            now_.unix_nanos() - record.verified_at.unix_nanos() >= Duration::from_minutes(5).nanos();
      }
      if (!needs_confirmation) {
        continue;
      }
      auto reference = ExternalEvidenceRef::parse(
          std::string("plant/confirm/") + std::to_string(record.request.id.value()));
      if (!reference.ok()) {
        return reference.status();
      }
      auto verified =
          runtime_.VerifyRequest(runtime_.Authority(), record.request.id, reference.value(), now_);
      if (!verified.ok()) {
        return verified.status();
      }
      ++outcome_.requests_verified;
      note(std::string("verified ") + std::string(mitigation_class_name(record.request.cls)) + " " +
           record.request.target.to_string());
    }
    return Status::success();
  }

  Status RelaxBlockingAdvisoryConstraints() {
    const ReadModel model = runtime_.Inspect(now_);
    if (model.lifecycle == Lifecycle::None || model.lifecycle == Lifecycle::Closed) {
      return Status::success();
    }
    const std::uint8_t required = required_class_mask(model.severity);
    if (required == 0) {
      return Status::success();
    }
    const std::array<MitigationClass, kMitigationClassCount> classes = {
        MitigationClass::DerateAccelerators, MitigationClass::DrainWorkload,
        MitigationClass::ReducePower, MitigationClass::IsolateEquipment};
    for (const MitigationClass cls : classes) {
      if ((required & mitigation_class_bit(cls)) == 0) {
        continue;
      }
      bool present = false;
      for (const MitigationRecord& record : model.requests) {
        if (record.request.cls == cls && (!record.is_terminal() || record.state == MitigationState::Verified)) {
          present = true;
          break;
        }
      }
      if (present) {
        continue;
      }
      for (const ProtectedObligation& obligation : model.obligations) {
        if (obligation.protection != ProtectionClass::AdvisoryOptimization ||
            !obligation_forbids(obligation, cls)) {
          continue;
        }
        auto granted = runtime_.RelaxConstraint(runtime_.Authority(), obligation.id, cls,
                                                ExternalAuthorityRef{std::string("ops/oncall")},
                                                now_);
        if (granted.ok()) {
          note("relaxed advisory constraint " + obligation.ref + " for " +
               std::string(mitigation_class_name(cls)));
        }
      }
    }
    return Status::success();
  }

  // Records every severity change with the modelled zone temperature, so a
  // scenario transcript shows what the facility was doing when the emergency
  // state moved.
  void NoteSeverityChange() {
    const ReadModel model = runtime_.Inspect(now_);
    if (model.severity == last_severity_) {
      return;
    }
    last_severity_ = model.severity;
    std::string text = std::string("severity ") + std::string(severity_name(model.severity));
    if (!plant_.zones().empty()) {
      text += " at modelled " +
              format_temperature(plant_.temperature_at(plant_.zones().front().spec.zone, now_));
    }
    note(std::move(text));
  }

  // The plant reacts only to verified mitigation, and only within the bounds of
  // the modelled class: a derate or drain slows the rise in the target zone.
  void ApplyMitigationEffects() {
    const ReadModel model = runtime_.Inspect(now_);
    std::map<std::string, std::int64_t> reduction;
    for (const MitigationRecord& record : model.requests) {
      if (record.state != MitigationState::Verified) {
        continue;
      }
      std::string zone;
      switch (record.request.cls) {
        case MitigationClass::DerateAccelerators:
          zone = record.request.target.zone().text();
          break;
        case MitigationClass::DrainWorkload:
          zone = record.request.target.zone().text();
          break;
        default:
          continue;
      }
      reduction[zone] = std::max<std::int64_t>(reduction[zone],
                                              static_cast<std::int64_t>(record.request.intensity.value()));
    }
    for (const SyntheticPlant::ZoneState& state : plant_.zones()) {
      const std::int64_t base = plant_.base_ramp(state.spec.zone).value();
      const auto it = reduction.find(state.spec.zone.text());
      if (it == reduction.end()) {
        plant_.SetEffectiveRamp(state.spec.zone,
                                MilliCelsiusPerMinute::from_value(static_cast<std::int32_t>(base)));
        continue;
      }
      // The requested reduction is a fraction of the unmitigated ramp; applying
      // it to the current ramp instead would compound it away within a few
      // ticks.
      const std::int64_t factor = 10000 - it->second;
      const std::int64_t adjusted = (base * factor) / 10000;
      plant_.SetEffectiveRamp(state.spec.zone,
                              MilliCelsiusPerMinute::from_value(static_cast<std::int32_t>(adjusted)));
    }
  }

  Status Run(Duration span) {
    const Timestamp end = now_.checked_add(span).value_or(now_);
    Timestamp next_tick = now_;
    while (now_ < end) {
      now_ = now_.checked_add(config_.sample_interval).value_or(now_);
      Status admitted = AdmitAll();
      if (!admitted.ok()) {
        return admitted;
      }
      if (!(now_ < next_tick)) {
        next_tick = now_.checked_add(config_.tick_interval).value_or(now_);
        Status ticked = Tick();
        if (!ticked.ok()) {
          return ticked;
        }
      }
    }
    return Status::success();
  }

  // Drives the incident until it is recovered, asking for recovery as soon as
  // the preconditions hold. Returns false when the span elapses first.
  Result<bool> RunUntilRecovered(Duration span) {
    const Timestamp end = now_.checked_add(span).value_or(now_);
    while (now_ < end) {
      now_ = now_.checked_add(config_.sample_interval).value_or(now_);
      Status admitted = AdmitAll();
      if (!admitted.ok()) {
        return admitted;
      }
      Status ticked = Tick();
      if (!ticked.ok()) {
        return ticked;
      }
      const ReadModel model = runtime_.Inspect(now_);
      if (model.lifecycle == Lifecycle::Stabilizing) {
        auto begun = runtime_.BeginRecovery(runtime_.Authority(),
                                            OperatorRef{std::string("ops/oncall")}, now_);
        if (!begun.ok()) {
          return begun.status();
        }
        note("recovery explicitly begun by operator authority");
      }
      if (model.lifecycle == Lifecycle::Recovered) {
        return true;
      }
    }
    return false;
  }

  Status Close() {
    // Closure requires the recovered state to have held for the closure dwell,
    // so the driver keeps the facility steady until the gate actually passes.
    const Timestamp limit = now_.checked_add(Duration::from_minutes(20)).value_or(now_);
    while (now_ < limit) {
      const ReadModel model = runtime_.Inspect(now_);
      if (model.lifecycle == Lifecycle::Stabilizing) {
        auto begun = runtime_.BeginRecovery(runtime_.Authority(),
                                            OperatorRef{std::string("ops/oncall")}, now_);
        if (!begun.ok()) {
          return begun.status();
        }
        note("recovery explicitly begun by operator authority");
      }
      if (model.lifecycle == Lifecycle::Recovered &&
          runtime_.EvaluateRecovery(now_, true).eligible) {
        break;
      }
      now_ = now_.checked_add(config_.sample_interval).value_or(now_);
      Status admitted = AdmitAll();
      if (!admitted.ok()) {
        return admitted;
      }
      Status ticked = Tick();
      if (!ticked.ok()) {
        return ticked;
      }
    }
    auto closed = runtime_.CloseIncident(runtime_.Authority(), OperatorRef{std::string("ops/oncall")},
                                         Disposition::Recovered, now_);
    return closed.status();
  }

  void RefreshOutcome() {
    const ReadModel model = runtime_.Inspect(now_);
    outcome_.final_model = model;
    outcome_.final_severity = model.severity;
    outcome_.final_lifecycle = model.lifecycle;
    outcome_.peak_severity = model.peak_severity;
    outcome_.recovery_eligible = model.eligibility.eligible;
    outcome_.closed = !model.incident_open && model.lifecycle == Lifecycle::Closed;
    outcome_.dispatches = static_cast<std::uint32_t>(transport_.dispatch_count());
    outcome_.requests_issued = 0;
    outcome_.requests_failed = 0;
    outcome_.requests_abandoned = 0;
    outcome_.requests_superseded = 0;
    for (const MitigationRecord& record : model.requests) {
      ++outcome_.requests_issued;
      switch (record.state) {
        case MitigationState::Failed:
        case MitigationState::Expired:
        case MitigationState::Refused:
          ++outcome_.requests_failed;
          break;
        case MitigationState::Abandoned:
          ++outcome_.requests_abandoned;
          break;
        case MitigationState::Superseded:
          ++outcome_.requests_superseded;
          break;
        default:
          break;
      }
    }
    if (model.requests.empty()) {
      outcome_.requests_verified = 0;
    }
  }

 private:
  EmergencyRuntime& runtime_;
  SyntheticPlant& plant_;
  const ScenarioConfig& config_;
  ScriptedTransport& transport_;
  ScenarioOutcome& outcome_;
  Timestamp now_;
  std::uint64_t sequence_{1};
  std::map<std::string, ObligationId> obligation_ids_{};
  Timestamp last_obligation_report_{};
  Severity last_severity_{Severity::Nominal};
};

bool fail(ScenarioOutcome& outcome, std::string detail) {
  outcome.ok = false;
  outcome.failure_detail = std::move(detail);
  return false;
}

}  // namespace

ScenarioRunner::ScenarioRunner(RuntimeFactory factory, ScenarioConfig config)
    : factory_(std::move(factory)), config_(std::move(config)) {}

Result<ScenarioOutcome> ScenarioRunner::Run(ScenarioId id) {
  ScenarioOutcome outcome;
  outcome.id = id;
  outcome.name = std::string(scenario_name(id));

  SyntheticPlant plant(config_.zones, config_.start);
  ScriptedTransport transport;

  ControllerIncarnation incarnation = ControllerIncarnation::from_value(1);
  ControlEpoch epoch = ControlEpoch::from_value(1);
  auto opened = factory_(incarnation, epoch, transport);
  if (!opened.ok()) {
    return opened.status();
  }
  std::unique_ptr<EmergencyRuntime> runtime = std::move(opened).value();

  Session session(*runtime, plant, config_, transport, outcome);
  // The active session follows the runtime. A restart destroys the previous
  // runtime, so the old session must never be used again.
  Session* active = &session;
  std::unique_ptr<Session> resumed_session;
  auto registered = session.RegisterFacility();
  if (!registered.ok()) {
    return registered;
  }

  const MilliCelsiusPerMinute full_rate = MilliCelsiusPerMinute::from_value(3'000);
  const MilliCelsiusPerMinute cooling_rate = MilliCelsiusPerMinute::from_value(-2'500);

  switch (id) {
    case ScenarioId::NominalSteady: {
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(0));
      auto status = session.Run(Duration::from_minutes(4));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = outcome.final_severity == Severity::Nominal &&
                   outcome.final_lifecycle == Lifecycle::None;
      if (!outcome.ok) {
        fail(outcome, "a steady nominal facility must not open an incident");
      }
      break;
    }

    case ScenarioId::RapidRise: {
      plant.SetRamp(config_.zones.front().zone, full_rate);
      auto status = session.Run(Duration::from_minutes(8));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = severity_ordinal(outcome.peak_severity) >= severity_ordinal(Severity::Critical) &&
                   outcome.requests_issued > 0;
      if (!outcome.ok) {
        fail(outcome, "a rapid rise must escalate to at least critical and issue requests");
      }
      break;
    }

    case ScenarioId::StaleSensors: {
      plant.SetRamp(config_.zones.front().zone, full_rate);
      auto status = session.Run(Duration::from_minutes(4));
      if (!status.ok()) {
        return status;
      }
      for (const ThermalProbeRef& probe : config_.zones.front().probes) {
        plant.FreezeProbe(probe);
      }
      session.note("all probes stopped reporting");
      status = session.Run(Duration::from_minutes(4));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      const bool blocked = !outcome.final_model.eligibility.eligible;
      outcome.ok = blocked && severity_ordinal(outcome.final_severity) >=
                                   severity_ordinal(Severity::Warning);
      if (!outcome.ok) {
        fail(outcome, "lost evidence must raise the severity floor and block recovery");
      }
      break;
    }

    case ScenarioId::ContradictorySensors: {
      const ZoneSpec& zone = config_.zones.front();
      plant.SetRamp(zone.zone, full_rate);
      plant.SetOffset(zone.probes.back(), MilliCelsius::from_value(9'000));
      auto status = session.Run(Duration::from_minutes(6));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      bool contradictory = false;
      for (const ZoneAssessment& assessment : outcome.final_model.evidence.zones) {
        contradictory = contradictory || assessment.cls == FreshnessClass::Contradictory;
      }
      outcome.ok = contradictory && !outcome.final_model.eligibility.eligible;
      if (!outcome.ok) {
        fail(outcome, "contradictory probes must be reported and must block recovery");
      }
      break;
    }

    case ScenarioId::PartialAcknowledgement: {
      // One class of the plan is accepted and another is refused: a partial
      // acknowledgement of the plan must never read as resolution.
      ScriptedTransport::Rule rule;
      rule.outcome = DispatchOutcome::Refused;
      rule.code = 7;
      rule.detail = "adjacent authority is not accepting this class";
      transport.SetRule(MitigationClass::DrainWorkload, rule);
      plant.SetRamp(config_.zones.front().zone, full_rate);
      auto status = session.Run(Duration::from_minutes(12));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = outcome.requests_failed > 0 && !outcome.final_model.eligibility.eligible;
      if (!outcome.ok) {
        fail(outcome, "a partially acknowledged plan must never count as resolved");
      }
      break;
    }

    case ScenarioId::FailedDrain: {
      ScriptedTransport::Rule rule;
      rule.outcome = DispatchOutcome::Error;
      rule.code = 11;
      rule.detail = "workload authority rejected the drain";
      transport.SetRule(MitigationClass::DrainWorkload, rule);
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(6'000));
      auto status = session.Run(Duration::from_minutes(12));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = count_requests(outcome.final_model, MitigationClass::DrainWorkload,
                                  MitigationState::Failed) > 0 &&
                   !outcome.final_model.eligibility.eligible;
      if (!outcome.ok) {
        fail(outcome, "a failed drain request must remain visible and block recovery");
      }
      break;
    }

    case ScenarioId::FailedPowerReduction: {
      ScriptedTransport::Rule rule;
      rule.outcome = DispatchOutcome::Refused;
      rule.code = 12;
      rule.detail = "power authority refused the reduction";
      transport.SetRule(MitigationClass::ReducePower, rule);
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(8'000));
      auto status = session.Run(Duration::from_minutes(12));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = count_requests(outcome.final_model, MitigationClass::ReducePower,
                                  MitigationState::Failed) > 0 &&
                   !outcome.final_model.eligibility.eligible;
      if (!outcome.ok) {
        fail(outcome, "a failed power-reduction request must block recovery");
      }
      break;
    }

    case ScenarioId::CoolingUnavailable: {
      const ZoneSpec& zone = config_.zones.front();
      plant.SetRamp(zone.zone, full_rate);
      auto status = session.Run(Duration::from_minutes(6));
      if (!status.ok()) {
        return status;
      }
      plant.SetQuality(zone.probes.front(), SampleQuality::Unavailable, SensorHealth::Degraded);
      session.note("cooling-domain evidence reports unavailable");
      status = session.Run(Duration::from_minutes(6));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = !outcome.final_model.eligibility.eligible &&
                   severity_ordinal(outcome.final_severity) >= severity_ordinal(Severity::Warning);
      if (!outcome.ok) {
        fail(outcome, "unavailable cooling evidence must not be read as recovery");
      }
      break;
    }

    case ScenarioId::IsolationSuccess: {
      // Forced hard enough that the modelled mitigations cannot contain it, so
      // the excursion reaches the level where isolation is required.
      plant.SetInitial(config_.zones.front().zone, MilliCelsius::from_value(50'000));
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(4'000));
      auto status = session.Run(Duration::from_minutes(9));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      session.note("modelled zone temperature: " +
                   format_temperature(
                       plant.temperature_at(config_.zones.front().zone, session.now())) +
                   " ramp=" + std::to_string(plant.zones().front().spec.ramp.value()) +
                   " ceiling=" + format_temperature(plant.zones().front().spec.ceiling) +
                   " initial=" + format_temperature(plant.zones().front().spec.initial));
      const bool catastrophic = outcome.peak_severity == Severity::Catastrophic;
      const bool isolated =
          count_requests(outcome.final_model, MitigationClass::IsolateEquipment,
                         MitigationState::Verified) > 0;
      plant.SetRamp(config_.zones.front().zone, cooling_rate);
      status = session.Run(Duration::from_minutes(10));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = catastrophic && isolated;
      if (!outcome.ok) {
        fail(outcome, "a catastrophic excursion must request and verify equipment isolation");
      }
      break;
    }

    case ScenarioId::IsolationFailure: {
      ScriptedTransport::Rule rule;
      rule.outcome = DispatchOutcome::Refused;
      rule.code = 21;
      rule.detail = "facility authority refused isolation";
      transport.SetRule(MitigationClass::IsolateEquipment, rule);
      plant.SetInitial(config_.zones.front().zone, MilliCelsius::from_value(50'000));
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(4'000));
      auto status = session.Run(Duration::from_minutes(9));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      outcome.ok = outcome.peak_severity == Severity::Catastrophic &&
                   count_requests(outcome.final_model, MitigationClass::IsolateEquipment,
                                  MitigationState::Failed) > 0 &&
                   !outcome.final_model.eligibility.eligible;
      if (!outcome.ok) {
        fail(outcome, "a refused isolation must leave the excursion unresolved");
      }
      break;
    }

    case ScenarioId::RestartMidEmergency:
    case ScenarioId::AuthorityRollover: {
      // The controller stops while mitigations are still awaiting confirmation,
      // which is exactly the state a rollover must fence.
      config_.verification_delay = Duration::from_minutes(10);
      plant.SetRamp(config_.zones.front().zone, full_rate);
      auto status = session.Run(Duration::from_minutes(7));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      const Severity before = outcome.final_severity;
      const std::uint64_t open_before = count_requests(outcome.final_model, MitigationState::Issued) +
                                        count_requests(outcome.final_model, MitigationState::Acknowledged) +
                                        count_requests(outcome.final_model, MitigationState::Observed) +
                                        count_requests(outcome.final_model, MitigationState::Planned);
      const Timestamp restart_at = session.now();
      session.note("controller stops mid-emergency");
      runtime->Shutdown();
      runtime.reset();

      incarnation = ControllerIncarnation::from_value(incarnation.value() + 1);
      epoch = ControlEpoch::from_value(epoch.value() + 1);
      auto reopened = factory_(incarnation, epoch, transport);
      if (!reopened.ok()) {
        return reopened.status();
      }
      runtime = std::move(reopened).value();
      ++outcome.restarts;
      resumed_session = std::make_unique<Session>(*runtime, plant, config_, transport, outcome,
                                                  restart_at);
      Session& resumed = *resumed_session;
      active = resumed_session.get();
      auto resumed_facility = resumed.RegisterFacility();
      if (!resumed_facility.ok()) {
        return resumed_facility;
      }
      resumed.note("controller restarted under a new incarnation and control epoch");
      const ReadModel restored = runtime->Inspect(session.now());
      bool fenced = restored.fenced_requests > 0 || open_before == 0;
      for (const MitigationRecord& record : restored.requests) {
        if (record.is_terminal()) {
          continue;
        }
        fenced = false;
      }
      const bool stale_evidence = !restored.evidence.all_fresh;
      resumed.RefreshOutcome();
      outcome.requests_superseded = restored.fenced_requests;
      auto continued = resumed.Run(Duration::from_minutes(3));
      if (!continued.ok()) {
        return continued;
      }
      resumed.RefreshOutcome();
      outcome.ok = fenced && stale_evidence && severity_ordinal(outcome.final_severity) >=
                                                     severity_ordinal(before);
      outcome.requests_superseded = restored.fenced_requests;
      if (!outcome.ok) {
        fail(outcome,
             "a restarted controller must fence the previous plan and require current evidence: "
             "fenced=" + std::to_string(fenced) + " stale_evidence=" +
                 std::to_string(stale_evidence) + " open_before=" + std::to_string(open_before) +
                 " fenced_requests=" + std::to_string(restored.fenced_requests) +
                 " before=" + std::string(severity_name(before)) + " final=" +
                 std::string(severity_name(outcome.final_severity)));
      }
      break;
    }

    case ScenarioId::RecoveryWithHysteresis: {
      plant.SetRamp(config_.zones.front().zone, full_rate);
      auto status = session.Run(Duration::from_minutes(6));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      const Severity peak = outcome.peak_severity;
      auto report = runtime->Tick(runtime->Authority(), session.now());
      if (!report.ok()) {
        return report.status();
      }
      if (report.value().lifecycle_before == Lifecycle::Active) {
        // The excursion is still rising; nothing to assert yet.
      }
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(-1'500));
      auto recovered = session.RunUntilRecovered(Duration::from_minutes(24));
      if (!recovered.ok()) {
        return recovered.status();
      }
      session.RefreshOutcome();
      const bool stepped = severity_ordinal(outcome.final_severity) < severity_ordinal(peak);
      bool no_skips = outcome.final_model.transitions.size() > 0;
      Severity previous = peak;
      for (const TransitionRecord& transition : outcome.final_model.transitions) {
        if (transition.from_lifecycle == Lifecycle::Recovering &&
            transition.to_severity != transition.from_severity) {
          no_skips = no_skips &&
                     severity_ordinal(transition.to_severity) + 1 == severity_ordinal(previous);
          previous = transition.to_severity;
        }
      }
      outcome.ok = stepped && no_skips;
      if (!outcome.ok) {
        fail(outcome, "cooling must permit stepwise de-escalation only under hysteresis: "
                          "peak=" + std::string(severity_name(peak)) + " final=" +
                          std::string(severity_name(outcome.final_severity)) + " lifecycle=" +
                          std::string(lifecycle_name(outcome.final_lifecycle)));
      }
      break;
    }

    case ScenarioId::ObligationBlocked: {
      plant.SetInitial(config_.zones.front().zone, MilliCelsius::from_value(50'000));
      plant.SetRamp(config_.zones.front().zone, MilliCelsiusPerMinute::from_value(4'000));
      auto status = session.Run(Duration::from_minutes(9));
      if (!status.ok()) {
        return status;
      }
      session.RefreshOutcome();
      const bool catastrophic = outcome.peak_severity == Severity::Catastrophic;
      const bool blocked_isolation =
          count_requests(outcome.final_model, MitigationClass::IsolateEquipment,
                         MitigationState::Failed) > 0 ||
          count_requests(outcome.final_model, MitigationClass::IsolateEquipment,
                         MitigationState::Refused) > 0;
      outcome.ok = catastrophic && !outcome.final_model.eligibility.eligible;
      if (blocked_isolation) {
        outcome.ok = outcome.ok && true;
      }
      if (!outcome.ok) {
        fail(outcome, "a protected obligation must keep the excursion unresolved");
      }
      break;
    }

    case ScenarioId::FullExcursion: {
      plant.SetRamp(config_.zones.front().zone, full_rate);
      auto status = session.Run(Duration::from_minutes(8));
      if (!status.ok()) {
        return status;
      }
      session.note("escalation phase complete");
      plant.SetRamp(config_.zones.front().zone, cooling_rate);
      status = session.Run(Duration::from_minutes(12));
      if (!status.ok()) {
        return status;
      }
      status = session.Close();
      if (!status.ok()) {
        // A closure that is refused is reported, not forced.
        session.RefreshOutcome();
        fail(outcome, "closure was refused: " + status.to_string());
        break;
      }
      session.note("incident closed after verified recovery");
      session.RefreshOutcome();
      outcome.ok = outcome.final_lifecycle == Lifecycle::Closed && outcome.dispatches > 0;
      if (!outcome.ok) {
        fail(outcome, "the end-to-end excursion must close after verified recovery");
      }
      break;
    }
  }

  active->RefreshOutcome();
  return outcome;
}

}  // namespace summon::tem
