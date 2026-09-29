// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <string>
#include <vector>

#include "tem/codec.hpp"
#include "tem/state.hpp"

namespace summon::tem::codec {
namespace {

constexpr std::size_t kMaxText = HardLimits::kMaxDescriptionLength;
constexpr std::uint32_t kMaxList = HardLimits::kMaxZones * 4u;

Status corrupt(const char* what) { return Status::error(StatusCode::StoreCorrupt, what); }

template <class Enum, class FromValue>
void decode_enum(Reader& reader, Enum& value, FromValue from_value, const char* what) {
  const std::uint8_t raw = reader.u8();
  if (!reader.ok()) {
    return;
  }
  const auto parsed = from_value(raw);
  if (!parsed.has_value()) {
    reader.fail(Status::error(StatusCode::UnknownEnumValue, what));
    return;
  }
  value = *parsed;
}

template <class T, class Fn>
void decode_list(Reader& reader, std::vector<T>& out, std::uint32_t limit, Fn decode_item) {
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return;
  }
  if (count > limit) {
    reader.fail(Status::error(StatusCode::BoundsExceeded,
                              "declared list length exceeds the structural bound"));
    return;
  }
  out.clear();
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    T item{};
    decode_item(item);
    if (!reader.ok()) {
      return;
    }
    out.push_back(std::move(item));
  }
}

void decode_ref(Reader& reader, RefToken& value) {
  const std::uint8_t raw_kind = reader.u8();
  std::string text;
  reader.text(text, kMaxRefLength);
  if (!reader.ok()) {
    return;
  }
  const auto kind = ref_kind_from_value(raw_kind);
  if (!kind.has_value()) {
    reader.fail(Status::error(StatusCode::UnknownEnumValue, "unknown reference kind"));
    return;
  }
  if (text.empty()) {
    // An empty token is how an absent optional reference is encoded. It is not
    // a malformed token, and it never compares equal to a present one.
    value = RefToken{*kind, std::string()};
    return;
  }
  auto canonical = canonical_ref_text(*kind, text);
  if (!canonical.ok()) {
    reader.fail(Status::error(StatusCode::StoreCorrupt,
                              "reference token in the store is not canonical"));
    return;
  }
  value = RefToken{*kind, std::move(canonical).value()};
}

}  // namespace

void encode(Writer& writer, const RefToken& value) {
  writer.u8(static_cast<std::uint8_t>(value.kind()));
  writer.text(value.text(), kMaxRefLength);
}

void decode(Reader& reader, RefToken& value) { decode_ref(reader, value); }

void encode(Writer& writer, Timestamp value) { writer.i64(value.unix_nanos()); }
void decode(Reader& reader, Timestamp& value) { value = Timestamp::from_unix_nanos(reader.i64()); }

void encode(Writer& writer, Duration value) { writer.i64(value.nanos()); }
void decode(Reader& reader, Duration& value) { value = Duration::from_nanos(reader.i64()); }

void encode(Writer& writer, MilliCelsius value) { writer.i32(value.value()); }
void decode(Reader& reader, MilliCelsius& value) {
  const std::int32_t raw = reader.i32();
  if (!reader.ok()) {
    return;
  }
  const auto checked = MilliCelsius::try_make(raw);
  if (!checked.has_value()) {
    reader.fail(Status::error(StatusCode::ValueOutOfRange, "temperature is out of range"));
    return;
  }
  value = *checked;
}

void encode(Writer& writer, MilliCelsiusPerMinute value) { writer.i32(value.value()); }
void decode(Reader& reader, MilliCelsiusPerMinute& value) {
  const std::int32_t raw = reader.i32();
  if (!reader.ok()) {
    return;
  }
  const auto checked = MilliCelsiusPerMinute::try_make(raw);
  if (!checked.has_value()) {
    reader.fail(Status::error(StatusCode::ValueOutOfRange, "rate is out of range"));
    return;
  }
  value = *checked;
}

void encode(Writer& writer, BasisPoints value) { writer.u16(value.value()); }
void decode(Reader& reader, BasisPoints& value) {
  const std::uint16_t raw = reader.u16();
  if (!reader.ok()) {
    return;
  }
  const auto checked = BasisPoints::try_make(raw);
  if (!checked.has_value()) {
    reader.fail(Status::error(StatusCode::ValueOutOfRange, "basis points are out of range"));
    return;
  }
  value = *checked;
}

void encode(Writer& writer, Fingerprint value) {
  writer.u64(value.high());
  writer.u64(value.low());
}
void decode(Reader& reader, Fingerprint& value) {
  const std::uint64_t high = reader.u64();
  const std::uint64_t low = reader.u64();
  value = Fingerprint{high, low};
}

void encode(Writer& writer, IdempotencyKey value) {
  writer.u64(value.high());
  writer.u64(value.low());
}
void decode(Reader& reader, IdempotencyKey& value) {
  const std::uint64_t high = reader.u64();
  const std::uint64_t low = reader.u64();
  value = IdempotencyKey{high, low};
}

void encode(Writer& writer, Severity value) { writer.u8(severity_ordinal(value)); }
void decode(Reader& reader, Severity& value) {
  decode_enum(reader, value, severity_from_value, "unknown severity value");
}

void encode(Writer& writer, Lifecycle value) { writer.u8(lifecycle_ordinal(value)); }
void decode(Reader& reader, Lifecycle& value) {
  decode_enum(reader, value, lifecycle_from_value, "unknown lifecycle value");
}

void encode(Writer& writer, Disposition value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, Disposition& value) {
  decode_enum(reader, value, disposition_from_value, "unknown disposition value");
}

void encode(Writer& writer, MitigationClass value) {
  writer.u8(mitigation_class_ordinal(value));
}
void decode(Reader& reader, MitigationClass& value) {
  decode_enum(reader, value, mitigation_class_from_value, "unknown mitigation class");
}

void encode(Writer& writer, MitigationState value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, MitigationState& value) {
  decode_enum(reader, value, mitigation_state_from_value, "unknown mitigation state");
}

void encode(Writer& writer, RequestReason value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, RequestReason& value) {
  decode_enum(reader, value, request_reason_from_value, "unknown request reason");
}

void encode(Writer& writer, FailureCode value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, FailureCode& value) {
  decode_enum(reader, value, failure_code_from_value, "unknown failure code");
}

void encode(Writer& writer, SupersedeReason value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, SupersedeReason& value) {
  decode_enum(reader, value, supersede_reason_from_value, "unknown supersede reason");
}

void encode(Writer& writer, SampleQuality value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, SampleQuality& value) {
  decode_enum(reader, value, sample_quality_from_value, "unknown sample quality");
}

void encode(Writer& writer, SensorHealth value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, SensorHealth& value) {
  decode_enum(reader, value, sensor_health_from_value, "unknown sensor health");
}

void encode(Writer& writer, SampleOrigin value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, SampleOrigin& value) {
  decode_enum(reader, value, sample_origin_from_value, "unknown sample origin");
}

void encode(Writer& writer, ProtectionClass value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, ProtectionClass& value) {
  decode_enum(reader, value, protection_class_from_value, "unknown protection class");
}

void encode(Writer& writer, ObligationStatus value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, ObligationStatus& value) {
  decode_enum(reader, value, obligation_status_from_value, "unknown obligation status");
}

void encode(Writer& writer, TransitionTrigger value) {
  writer.u8(static_cast<std::uint8_t>(value));
}
void decode(Reader& reader, TransitionTrigger& value) {
  decode_enum(reader, value, transition_trigger_from_value, "unknown transition trigger");
}

void encode(Writer& writer, JournalKind value) { writer.u8(static_cast<std::uint8_t>(value)); }
void decode(Reader& reader, JournalKind& value) {
  decode_enum(reader, value, journal_kind_from_value, "unknown journal kind");
}

void encode(Writer& writer, const RequestTarget& value) {
  encode(writer, value.token());
}
void decode(Reader& reader, RequestTarget& value) {
  RefToken token;
  decode_ref(reader, token);
  if (!reader.ok()) {
    return;
  }
  switch (token.kind()) {
    case RefKind::ThermalZone:
      value = RequestTarget::for_zone(ThermalZoneRef{token.text()});
      break;
    case RefKind::PowerDomain:
      value = RequestTarget::for_power_domain(PowerDomainRef{token.text()});
      break;
    case RefKind::Equipment:
      value = RequestTarget::for_equipment(EquipmentRef{token.text()});
      break;
    default:
      reader.fail(corrupt("request target uses a reference kind that cannot be a target"));
      return;
  }
}

void encode(Writer& writer, const ThermalSample& value) {
  encode(writer, value.zone.token());
  encode(writer, value.probe.token());
  encode(writer, value.source.token());
  encode(writer, value.quality);
  encode(writer, value.health);
  encode(writer, value.origin);
  writer.flag(value.has_temperature);
  encode(writer, value.temperature);
  writer.flag(value.has_rate);
  encode(writer, value.rate);
  encode(writer, value.observed_at);
  encode(writer, value.received_at);
  writer.u64(value.generation.value());
  writer.u64(value.sequence.value());
}

void decode(Reader& reader, ThermalSample& value) {
  RefToken zone;
  RefToken probe;
  RefToken source;
  decode_ref(reader, zone);
  decode_ref(reader, probe);
  decode_ref(reader, source);
  if (!reader.ok()) {
    return;
  }
  if (zone.kind() != RefKind::ThermalZone || probe.kind() != RefKind::ThermalProbe ||
      source.kind() != RefKind::SensorSource) {
    reader.fail(corrupt("sample carries a reference of the wrong kind"));
    return;
  }
  value.zone = ThermalZoneRef{zone.text()};
  value.probe = ThermalProbeRef{probe.text()};
  value.source = SensorSourceRef{source.text()};
  decode(reader, value.quality);
  decode(reader, value.health);
  decode(reader, value.origin);
  value.has_temperature = reader.flag();
  decode(reader, value.temperature);
  value.has_rate = reader.flag();
  decode(reader, value.rate);
  decode(reader, value.observed_at);
  decode(reader, value.received_at);
  value.generation = EvidenceGeneration::from_value(reader.u64());
  value.sequence = ObservationSequence::from_value(reader.u64());
}

void encode(Writer& writer, const MitigationRequest& value) {
  writer.u64(value.id.value());
  writer.u64(value.attempt.value());
  writer.u64(value.command.value());
  writer.u64(value.incident.value());
  writer.u64(value.incident_generation.value());
  writer.u64(value.epoch.value());
  writer.u64(value.incarnation.value());
  writer.u64(value.planned_revision.value());
  encode(writer, value.cls);
  encode(writer, value.target);
  encode(writer, value.intensity);
  encode(writer, value.reason);
  writer.count(static_cast<std::uint32_t>(value.evidence_refs.size()));
  for (const RefToken& ref : value.evidence_refs) {
    encode(writer, ref);
  }
  encode(writer, value.fingerprint);
  encode(writer, value.key);
  encode(writer, value.created_at);
  encode(writer, value.expires_at);
  writer.flag(value.has_expiry);
  encode(writer, value.verification_deadline);
  writer.flag(value.has_verification_deadline);
}

void decode(Reader& reader, MitigationRequest& value) {
  value.id = MitigationRequestId::from_value(reader.u64());
  value.attempt = AttemptId::from_value(reader.u64());
  value.command = CommandId::from_value(reader.u64());
  value.incident = IncidentId::from_value(reader.u64());
  value.incident_generation = IncidentGeneration::from_value(reader.u64());
  value.epoch = ControlEpoch::from_value(reader.u64());
  value.incarnation = ControllerIncarnation::from_value(reader.u64());
  value.planned_revision = StateRevision::from_value(reader.u64());
  decode(reader, value.cls);
  decode(reader, value.target);
  decode(reader, value.intensity);
  decode(reader, value.reason);
  decode_list<RefToken>(
      reader, value.evidence_refs, HardLimits::kMaxEvidenceRefs,
      [&reader](RefToken& item) { decode_ref(reader, item); });
  decode(reader, value.fingerprint);
  decode(reader, value.key);
  decode(reader, value.created_at);
  decode(reader, value.expires_at);
  value.has_expiry = reader.flag();
  decode(reader, value.verification_deadline);
  value.has_verification_deadline = reader.flag();
}

void encode(Writer& writer, const RequestHistoryPoint& value) {
  encode(writer, value.state);
  encode(writer, value.at);
  encode(writer, value.failure);
  encode(writer, value.supersede);
  writer.i32(value.detail);
}

void decode(Reader& reader, RequestHistoryPoint& value) {
  decode(reader, value.state);
  decode(reader, value.at);
  decode(reader, value.failure);
  decode(reader, value.supersede);
  value.detail = reader.i32();
}

void encode(Writer& writer, const MitigationRecord& value) {
  encode(writer, value.request);
  encode(writer, value.state);
  encode(writer, value.state_at);
  encode(writer, value.failure);
  encode(writer, value.supersede);
  encode(writer, value.external_ref.token());
  writer.flag(value.has_external_ref);
  writer.u64(value.last_observation.value());
  encode(writer, value.verified_at);
  writer.flag(value.has_verified_at);
  encode(writer, value.verification_ref.token());
  writer.flag(value.has_verification_ref);
  writer.count(static_cast<std::uint32_t>(value.history.size()));
  for (const RequestHistoryPoint& point : value.history) {
    encode(writer, point);
  }
  writer.i32(value.transport_code);
  writer.flag(value.recovered_from_store);
}

void decode(Reader& reader, MitigationRecord& value) {
  decode(reader, value.request);
  decode(reader, value.state);
  decode(reader, value.state_at);
  decode(reader, value.failure);
  decode(reader, value.supersede);
  RefToken external;
  decode_ref(reader, external);
  if (reader.ok() && external.valid() && external.kind() != RefKind::ExternalEvidence) {
    reader.fail(corrupt("external reference has the wrong kind"));
    return;
  }
  if (external.valid()) {
    value.external_ref = ExternalEvidenceRef{external.text()};
  }
  value.has_external_ref = reader.flag();
  value.last_observation = ObservationSequence::from_value(reader.u64());
  decode(reader, value.verified_at);
  value.has_verified_at = reader.flag();
  RefToken verification;
  decode_ref(reader, verification);
  if (!reader.ok()) {
    return;
  }
  if (verification.valid()) {
    value.verification_ref = ExternalEvidenceRef{verification.text()};
  }
  value.has_verification_ref = reader.flag();
  decode_list<RequestHistoryPoint>(
      reader, value.history, HardLimits::kMaxRequestHistory,
      [&reader](RequestHistoryPoint& item) { decode(reader, item); });
  value.transport_code = reader.i32();
  value.recovered_from_store = reader.flag();
}

void encode(Writer& writer, const ProtectedObligation& value) {
  writer.u64(value.id.value());
  writer.text(value.ref, kMaxRefLength);
  encode(writer, value.zone.token());
  writer.text(value.description, kMaxText);
  encode(writer, value.protection);
  writer.u8(value.forbidden_classes);
  encode(writer, value.authority.token());
  encode(writer, value.status);
  encode(writer, value.status_at);
  writer.flag(value.status_present);
  encode(writer, value.status_source.token());
}

void decode(Reader& reader, ProtectedObligation& value) {
  value.id = ObligationId::from_value(reader.u64());
  reader.text(value.ref, kMaxRefLength);
  RefToken zone;
  decode_ref(reader, zone);
  if (reader.ok() && zone.valid() && zone.kind() != RefKind::ThermalZone) {
    reader.fail(corrupt("obligation zone reference has the wrong kind"));
    return;
  }
  if (zone.valid()) {
    value.zone = ThermalZoneRef{zone.text()};
  }
  reader.text(value.description, kMaxText);
  decode(reader, value.protection);
  value.forbidden_classes = reader.u8();
  RefToken authority;
  decode_ref(reader, authority);
  if (!reader.ok()) {
    return;
  }
  if (authority.valid()) {
    value.authority = ExternalAuthorityRef{authority.text()};
  }
  decode(reader, value.status);
  decode(reader, value.status_at);
  value.status_present = reader.flag();
  RefToken source;
  decode_ref(reader, source);
  if (!reader.ok()) {
    return;
  }
  if (source.valid()) {
    value.status_source = ExternalAuthorityRef{source.text()};
  }
}

void encode(Writer& writer, const RelaxationRecord& value) {
  writer.u64(value.obligation.value());
  encode(writer, value.cls);
  encode(writer, value.granted_by.token());
  encode(writer, value.granted_at);
  writer.flag(value.active);
}

void decode(Reader& reader, RelaxationRecord& value) {
  value.obligation = ObligationId::from_value(reader.u64());
  decode(reader, value.cls);
  RefToken granted_by;
  decode_ref(reader, granted_by);
  if (reader.ok() && granted_by.valid()) {
    value.granted_by = ExternalAuthorityRef{granted_by.text()};
  }
  decode(reader, value.granted_at);
  value.active = reader.flag();
}

void encode(Writer& writer, const TransitionRecord& value) {
  writer.u64(value.sequence.value());
  encode(writer, value.at);
  writer.u64(value.revision.value());
  writer.u64(value.incident.value());
  encode(writer, value.from_severity);
  encode(writer, value.to_severity);
  encode(writer, value.from_lifecycle);
  encode(writer, value.to_lifecycle);
  encode(writer, value.trigger);
  writer.text(value.detail, kMaxText);
}

void decode(Reader& reader, TransitionRecord& value) {
  value.sequence = TransitionSequence::from_value(reader.u64());
  decode(reader, value.at);
  value.revision = StateRevision::from_value(reader.u64());
  value.incident = IncidentId::from_value(reader.u64());
  decode(reader, value.from_severity);
  decode(reader, value.to_severity);
  decode(reader, value.from_lifecycle);
  decode(reader, value.to_lifecycle);
  decode(reader, value.trigger);
  reader.text(value.detail, kMaxText);
}

void encode(Writer& writer, const ProbeSlot& value) {
  encode(writer, value.probe.token());
  writer.flag(value.present);
  writer.flag(value.live_since_recovery);
  encode(writer, value.sample);
  writer.u64(value.recorded_at.value());
  writer.u64(value.last_sequence.value());
}

void decode(Reader& reader, ProbeSlot& value) {
  RefToken probe;
  decode_ref(reader, probe);
  if (reader.ok() && probe.valid()) {
    if (probe.kind() != RefKind::ThermalProbe) {
      reader.fail(corrupt("probe slot holds a reference of the wrong kind"));
      return;
    }
    value.probe = ThermalProbeRef{probe.text()};
  }
  value.present = reader.flag();
  value.live_since_recovery = reader.flag();
  decode(reader, value.sample);
  value.recorded_at = JournalSequence::from_value(reader.u64());
  value.last_sequence = ObservationSequence::from_value(reader.u64());
}

void encode(Writer& writer, const ZoneSlot& value) {
  encode(writer, value.zone.token());
  writer.count(static_cast<std::uint32_t>(value.probes.size()));
  for (const ProbeSlot& probe : value.probes) {
    encode(writer, probe);
  }
}

void decode(Reader& reader, ZoneSlot& value) {
  RefToken zone;
  decode_ref(reader, zone);
  if (reader.ok() && zone.valid()) {
    if (zone.kind() != RefKind::ThermalZone) {
      reader.fail(corrupt("zone slot holds a reference of the wrong kind"));
      return;
    }
    value.zone = ThermalZoneRef{zone.text()};
  }
  decode_list<ProbeSlot>(reader, value.probes, HardLimits::kMaxProbesPerZone,
                         [&reader](ProbeSlot& item) { decode(reader, item); });
}

void encode(Writer& writer, const ObligationSlot& value) { encode(writer, value.obligation); }
void decode(Reader& reader, ObligationSlot& value) { decode(reader, value.obligation); }

void encode(Writer& writer, const IncidentProjection& value) {
  writer.u64(value.id.value());
  writer.u64(value.generation.value());
  encode(writer, value.trigger_zone.token());
  encode(writer, value.severity);
  encode(writer, value.peak_severity);
  encode(writer, value.lifecycle);
  encode(writer, value.opened_at);
  encode(writer, value.closed_at);
  writer.flag(value.closed);
  writer.flag(value.margin_ok);
  encode(writer, value.margin_since);
  writer.flag(value.recovered_ok);
  encode(writer, value.recovered_since);
  encode(writer, value.stabilizing_since);
  encode(writer, value.last_escalation_at);
  writer.flag(value.has_last_escalation);
  encode(writer, value.last_deescalation_at);
  writer.flag(value.has_last_deescalation);
  encode(writer, value.began_recovery_at);
  writer.flag(value.has_begun_recovery);
  writer.count(static_cast<std::uint32_t>(value.requests.size()));
  for (const MitigationRecord& record : value.requests) {
    encode(writer, record);
  }
  writer.count(static_cast<std::uint32_t>(value.relaxations.size()));
  for (const RelaxationRecord& relaxation : value.relaxations) {
    encode(writer, relaxation);
  }
  writer.count(static_cast<std::uint32_t>(value.transitions.size()));
  for (const TransitionRecord& transition : value.transitions) {
    encode(writer, transition);
  }
  writer.u64(value.transitions_dropped);
  writer.u64(value.fenced_requests);
  encode(writer, value.disposition);
  writer.flag(value.has_disposition);
  encode(writer, value.closed_by.token());
}

void decode(Reader& reader, IncidentProjection& value) {
  value.id = IncidentId::from_value(reader.u64());
  value.generation = IncidentGeneration::from_value(reader.u64());
  RefToken trigger;
  decode_ref(reader, trigger);
  if (reader.ok() && trigger.valid()) {
    if (trigger.kind() != RefKind::ThermalZone) {
      reader.fail(corrupt("incident trigger holds a reference of the wrong kind"));
      return;
    }
    value.trigger_zone = ThermalZoneRef{trigger.text()};
  }
  decode(reader, value.severity);
  decode(reader, value.peak_severity);
  decode(reader, value.lifecycle);
  decode(reader, value.opened_at);
  decode(reader, value.closed_at);
  value.closed = reader.flag();
  value.margin_ok = reader.flag();
  decode(reader, value.margin_since);
  value.recovered_ok = reader.flag();
  decode(reader, value.recovered_since);
  decode(reader, value.stabilizing_since);
  decode(reader, value.last_escalation_at);
  value.has_last_escalation = reader.flag();
  decode(reader, value.last_deescalation_at);
  value.has_last_deescalation = reader.flag();
  decode(reader, value.began_recovery_at);
  value.has_begun_recovery = reader.flag();
  decode_list<MitigationRecord>(reader, value.requests, HardLimits::kMaxRequestsPerIncident,
                                [&reader](MitigationRecord& item) { decode(reader, item); });
  decode_list<RelaxationRecord>(reader, value.relaxations, HardLimits::kMaxRelaxationsPerIncident,
                                [&reader](RelaxationRecord& item) { decode(reader, item); });
  decode_list<TransitionRecord>(reader, value.transitions, HardLimits::kMaxTransitionsPerIncident,
                                [&reader](TransitionRecord& item) { decode(reader, item); });
  value.transitions_dropped = reader.u64();
  value.fenced_requests = reader.u64();
  decode(reader, value.disposition);
  value.has_disposition = reader.flag();
  RefToken closed_by;
  decode_ref(reader, closed_by);
  if (reader.ok() && closed_by.valid()) {
    value.closed_by = OperatorRef{closed_by.text()};
  }
}

void encode(Writer& writer, const Bounds& value) {
  writer.u32(value.max_zones);
  writer.u32(value.max_probes_per_zone);
  writer.u32(value.max_obligations);
  writer.u32(value.max_requests_per_incident);
  writer.u32(value.max_request_history);
  writer.u32(value.max_evidence_refs);
  writer.u32(value.max_journal_entries);
  writer.u32(value.max_incidents);
  writer.u32(value.max_transitions_per_incident);
  writer.u32(value.max_relaxations_per_incident);
  writer.u32(value.max_description_length);
  writer.u32(value.max_requests_per_issue);
}

void decode(Reader& reader, Bounds& value) {
  value.max_zones = reader.u32();
  value.max_probes_per_zone = reader.u32();
  value.max_obligations = reader.u32();
  value.max_requests_per_incident = reader.u32();
  value.max_request_history = reader.u32();
  value.max_evidence_refs = reader.u32();
  value.max_journal_entries = reader.u32();
  value.max_incidents = reader.u32();
  value.max_transitions_per_incident = reader.u32();
  value.max_relaxations_per_incident = reader.u32();
  value.max_description_length = reader.u32();
  value.max_requests_per_issue = reader.u32();
}

void encode(Writer& writer, const ThermalPolicy& value) {
  writer.u64(value.generation.value());
  encode(writer, value.advisory_enter);
  encode(writer, value.warning_enter);
  encode(writer, value.critical_enter);
  encode(writer, value.emergency_enter);
  encode(writer, value.catastrophic_enter);
  encode(writer, value.rapid_rate);
  encode(writer, value.extreme_rate);
  encode(writer, value.rapid_rate_floor);
  encode(writer, value.extreme_rate_floor);
  encode(writer, value.recovery_margin);
  encode(writer, value.recovery_dwell);
  encode(writer, value.deescalation_dwell);
  encode(writer, value.closure_dwell);
  encode(writer, value.evidence_staleness);
  encode(writer, value.evidence_future_skew);
  encode(writer, value.contradiction_tolerance);
  encode(writer, value.request_validity);
  encode(writer, value.verification_deadline);
  encode(writer, value.verification_validity);
  writer.flag(value.escalate_on_evidence_loss);
  encode(writer, value.bounds);
}

void decode(Reader& reader, ThermalPolicy& value) {
  value.generation = PolicyGeneration::from_value(reader.u64());
  decode(reader, value.advisory_enter);
  decode(reader, value.warning_enter);
  decode(reader, value.critical_enter);
  decode(reader, value.emergency_enter);
  decode(reader, value.catastrophic_enter);
  decode(reader, value.rapid_rate);
  decode(reader, value.extreme_rate);
  decode(reader, value.rapid_rate_floor);
  decode(reader, value.extreme_rate_floor);
  decode(reader, value.recovery_margin);
  decode(reader, value.recovery_dwell);
  decode(reader, value.deescalation_dwell);
  decode(reader, value.closure_dwell);
  decode(reader, value.evidence_staleness);
  decode(reader, value.evidence_future_skew);
  decode(reader, value.contradiction_tolerance);
  decode(reader, value.request_validity);
  decode(reader, value.verification_deadline);
  decode(reader, value.verification_validity);
  value.escalate_on_evidence_loss = reader.flag();
  decode(reader, value.bounds);
}

void encode(Writer& writer, const RelaxationPolicy& value) {
  writer.u64(value.generation.value());
  writer.flag(value.allow_advisory_relaxation);
  writer.u8(value.relaxable_classes);
}

void decode(Reader& reader, RelaxationPolicy& value) {
  value.generation = PolicyGeneration::from_value(reader.u64());
  value.allow_advisory_relaxation = reader.flag();
  value.relaxable_classes = reader.u8();
}

void encode(Writer& writer, const DomainState& value) {
  writer.u64(value.incarnation.value());
  writer.u64(value.epoch.value());
  writer.u64(value.revision.value());
  writer.u64(value.journal_sequence.value());
  writer.u64(value.transition_sequence.value());
  writer.u64(value.next_incident_id.value());
  writer.u64(value.next_generation.value());
  writer.u64(value.next_request_id.value());
  writer.u64(value.next_command_id.value());
  writer.u64(value.next_attempt_id.value());
  writer.u64(value.next_obligation_id.value());
  writer.u64(value.current_incident.value());
  writer.u64(value.current_generation.value());
  writer.count(static_cast<std::uint32_t>(value.zones.size()));
  for (const ZoneSlot& zone : value.zones) {
    encode(writer, zone);
  }
  writer.count(static_cast<std::uint32_t>(value.obligations.size()));
  for (const ObligationSlot& obligation : value.obligations) {
    encode(writer, obligation);
  }
  writer.count(static_cast<std::uint32_t>(value.bindings.size()));
  for (const TargetBinding& binding : value.bindings) {
    encode(writer, binding.zone.token());
    encode(writer, binding.power_domain.token());
    writer.count(static_cast<std::uint32_t>(binding.equipment.size()));
    for (const EquipmentRef& equipment : binding.equipment) {
      encode(writer, equipment.token());
    }
  }
  writer.count(static_cast<std::uint32_t>(value.incidents.size()));
  for (const IncidentProjection& incident : value.incidents) {
    encode(writer, incident);
  }
  encode(writer, value.policy);
  encode(writer, value.relaxation);
  writer.flag(value.recovered_from_store);
  encode(writer, value.last_input_at);
  writer.u32(0);  // reserved for future layout extensions; must decode as zero
}

void decode(Reader& reader, DomainState& value) {
  value.incarnation = ControllerIncarnation::from_value(reader.u64());
  value.epoch = ControlEpoch::from_value(reader.u64());
  value.revision = StateRevision::from_value(reader.u64());
  value.journal_sequence = JournalSequence::from_value(reader.u64());
  value.transition_sequence = TransitionSequence::from_value(reader.u64());
  value.next_incident_id = IncidentId::from_value(reader.u64());
  value.next_generation = IncidentGeneration::from_value(reader.u64());
  value.next_request_id = MitigationRequestId::from_value(reader.u64());
  value.next_command_id = CommandId::from_value(reader.u64());
  value.next_attempt_id = AttemptId::from_value(reader.u64());
  value.next_obligation_id = ObligationId::from_value(reader.u64());
  value.current_incident = IncidentId::from_value(reader.u64());
  value.current_generation = IncidentGeneration::from_value(reader.u64());
  decode_list<ZoneSlot>(reader, value.zones, HardLimits::kMaxZones,
                        [&reader](ZoneSlot& item) { decode(reader, item); });
  decode_list<ObligationSlot>(reader, value.obligations, HardLimits::kMaxObligations,
                              [&reader](ObligationSlot& item) { decode(reader, item); });
  const std::uint32_t binding_count = reader.u32();
  if (!reader.ok()) {
    return;
  }
  if (binding_count > HardLimits::kMaxZones) {
    reader.fail(Status::error(StatusCode::BoundsExceeded, "binding count exceeds the hard limit"));
    return;
  }
  value.bindings.clear();
  value.bindings.reserve(binding_count);
  for (std::uint32_t i = 0; i < binding_count; ++i) {
    RefToken zone;
    RefToken domain;
    decode_ref(reader, zone);
    decode_ref(reader, domain);
    if (!reader.ok()) {
      return;
    }
    if (zone.kind() != RefKind::ThermalZone || domain.kind() != RefKind::PowerDomain) {
      reader.fail(corrupt("target binding holds a reference of the wrong kind"));
      return;
    }
    TargetBinding binding;
    binding.zone = ThermalZoneRef{zone.text()};
    if (domain.valid()) {
      binding.power_domain = PowerDomainRef{domain.text()};
    }
    const std::uint32_t equipment_count = reader.u32();
    if (!reader.ok()) {
      return;
    }
    if (equipment_count > HardLimits::kMaxProbesPerZone) {
      reader.fail(Status::error(StatusCode::BoundsExceeded,
                                "equipment count exceeds the hard limit"));
      return;
    }
    binding.equipment.reserve(equipment_count);
    for (std::uint32_t e = 0; e < equipment_count; ++e) {
      RefToken equipment;
      decode_ref(reader, equipment);
      if (!reader.ok()) {
        return;
      }
      if (equipment.kind() != RefKind::Equipment) {
        reader.fail(corrupt("equipment reference has the wrong kind"));
        return;
      }
      binding.equipment.push_back(EquipmentRef{equipment.text()});
    }
    value.bindings.push_back(std::move(binding));
  }
  decode_list<IncidentProjection>(reader, value.incidents, HardLimits::kMaxIncidents,
                                  [&reader](IncidentProjection& item) { decode(reader, item); });
  decode(reader, value.policy);
  decode(reader, value.relaxation);
  value.recovered_from_store = reader.flag();
  decode(reader, value.last_input_at);
  const std::uint32_t reserved = reader.u32();
  if (!reader.ok()) {
    return;
  }
  if (reserved != 0) {
    reader.fail(Status::error(StatusCode::ReservedFieldNotZero,
                              "reserved domain-state field is not zero"));
    return;
  }
}

void encode(Writer& writer, const JournalPayload& value) {
  writer.u8(static_cast<std::uint8_t>(value.index() + 1u));
  std::visit(
      [&writer](const auto& payload) {
        using PayloadType = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<PayloadType, IncidentOpenedPayload>) {
          encode(writer, payload.trigger_zone.token());
          encode(writer, payload.initial);
        } else if constexpr (std::is_same_v<PayloadType, ZoneRegisteredPayload>) {
          encode(writer, payload.zone.token());
        } else if constexpr (std::is_same_v<PayloadType, ProbeRegisteredPayload>) {
          encode(writer, payload.zone.token());
          encode(writer, payload.probe.token());
        } else if constexpr (std::is_same_v<PayloadType, ObligationRegisteredPayload>) {
          encode(writer, payload.obligation);
        } else if constexpr (std::is_same_v<PayloadType, EvidenceAdmittedPayload>) {
          encode(writer, payload.sample);
        } else if constexpr (std::is_same_v<PayloadType, ObligationStatusPayload>) {
          writer.u64(payload.obligation.value());
          encode(writer, payload.status);
          encode(writer, payload.status_at);
          encode(writer, payload.source.token());
        } else if constexpr (std::is_same_v<PayloadType, TickPayload>) {
          writer.u32(payload.evaluated_zones);
        } else if constexpr (std::is_same_v<PayloadType, RequestsIssuedPayload>) {
          writer.count(static_cast<std::uint32_t>(payload.requests.size()));
          for (const MitigationRequest& request : payload.requests) {
            encode(writer, request);
          }
        } else if constexpr (std::is_same_v<PayloadType, RequestAcknowledgedPayload>) {
          writer.u64(payload.request.value());
          encode(writer, payload.external_ref.token());
        } else if constexpr (std::is_same_v<PayloadType, RequestObservedPayload>) {
          writer.u64(payload.request.value());
          encode(writer, payload.external_ref.token());
          writer.u64(payload.observation.value());
        } else if constexpr (std::is_same_v<PayloadType, RequestVerifiedPayload>) {
          writer.u64(payload.request.value());
          encode(writer, payload.verification_ref.token());
        } else if constexpr (std::is_same_v<PayloadType, RequestFailedPayload>) {
          writer.u64(payload.request.value());
          encode(writer, payload.failure);
          writer.i32(payload.transport_code);
        } else if constexpr (std::is_same_v<PayloadType, RequestSupersededPayload>) {
          writer.u64(payload.request.value());
          encode(writer, payload.reason);
        } else if constexpr (std::is_same_v<PayloadType, RequestAbandonedPayload>) {
          writer.u64(payload.request.value());
        } else if constexpr (std::is_same_v<PayloadType, ConstraintRelaxedPayload>) {
          writer.u64(payload.obligation.value());
          encode(writer, payload.cls);
          encode(writer, payload.authority.token());
        } else if constexpr (std::is_same_v<PayloadType, ConstraintReimposedPayload>) {
          writer.u64(payload.obligation.value());
        } else if constexpr (std::is_same_v<PayloadType, RecoveryBegunPayload>) {
          encode(writer, payload.operator_ref.token());
        } else if constexpr (std::is_same_v<PayloadType, IncidentClosedPayload>) {
          encode(writer, payload.operator_ref.token());
          encode(writer, payload.disposition);
        } else if constexpr (std::is_same_v<PayloadType, AuthorityRolledOverPayload>) {
          writer.u64(payload.incarnation.value());
          writer.u64(payload.epoch.value());
        } else if constexpr (std::is_same_v<PayloadType, PolicyAdoptedPayload>) {
          encode(writer, payload.policy);
          encode(writer, payload.relaxation);
        } else if constexpr (std::is_same_v<PayloadType, TargetBindingRegisteredPayload>) {
          encode(writer, payload.zone.token());
          encode(writer, payload.power_domain.token());
          writer.count(static_cast<std::uint32_t>(payload.equipment.size()));
          for (const EquipmentRef& equipment : payload.equipment) {
            encode(writer, equipment.token());
          }
        } else if constexpr (std::is_same_v<PayloadType, StoreRecoveredPayload>) {
          writer.flag(payload.rollover);
          writer.u64(payload.previous_incarnation.value());
          writer.u64(payload.previous_epoch.value());
        }
      },
      value);
}

void decode(Reader& reader, JournalPayload& value) {
  const std::uint8_t kind = reader.u8();
  if (!reader.ok()) {
    return;
  }
  switch (kind) {
    case 1: {
      IncidentOpenedPayload payload;
      RefToken zone;
      decode_ref(reader, zone);
      if (reader.ok() && zone.valid()) {
        payload.trigger_zone = ThermalZoneRef{zone.text()};
      }
      decode(reader, payload.initial);
      value = std::move(payload);
      break;
    }
    case 2: {
      ZoneRegisteredPayload payload;
      RefToken zone;
      decode_ref(reader, zone);
      if (reader.ok() && zone.valid()) {
        payload.zone = ThermalZoneRef{zone.text()};
      }
      value = std::move(payload);
      break;
    }
    case 3: {
      ProbeRegisteredPayload payload;
      RefToken zone;
      RefToken probe;
      decode_ref(reader, zone);
      decode_ref(reader, probe);
      if (reader.ok()) {
        if (zone.valid()) payload.zone = ThermalZoneRef{zone.text()};
        if (probe.valid()) payload.probe = ThermalProbeRef{probe.text()};
      }
      value = std::move(payload);
      break;
    }
    case 4: {
      ObligationRegisteredPayload payload;
      decode(reader, payload.obligation);
      value = std::move(payload);
      break;
    }
    case 5: {
      EvidenceAdmittedPayload payload;
      decode(reader, payload.sample);
      value = std::move(payload);
      break;
    }
    case 6: {
      ObligationStatusPayload payload;
      payload.obligation = ObligationId::from_value(reader.u64());
      decode(reader, payload.status);
      decode(reader, payload.status_at);
      RefToken source;
      decode_ref(reader, source);
      if (reader.ok() && source.valid()) {
        payload.source = ExternalAuthorityRef{source.text()};
      }
      value = std::move(payload);
      break;
    }
    case 7: {
      TickPayload payload;
      payload.evaluated_zones = reader.u32();
      value = std::move(payload);
      break;
    }
    case 8: {
      RequestsIssuedPayload payload;
      decode_list<MitigationRequest>(reader, payload.requests, HardLimits::kMaxRequestsPerIssue,
                                     [&reader](MitigationRequest& item) { decode(reader, item); });
      value = std::move(payload);
      break;
    }
    case 9: {
      RequestAcknowledgedPayload payload;
      payload.request = MitigationRequestId::from_value(reader.u64());
      RefToken ref;
      decode_ref(reader, ref);
      if (reader.ok() && ref.valid()) {
        payload.external_ref = ExternalEvidenceRef{ref.text()};
      }
      value = std::move(payload);
      break;
    }
    case 10: {
      RequestObservedPayload payload;
      payload.request = MitigationRequestId::from_value(reader.u64());
      RefToken ref;
      decode_ref(reader, ref);
      if (reader.ok() && ref.valid()) {
        payload.external_ref = ExternalEvidenceRef{ref.text()};
      }
      payload.observation = ObservationSequence::from_value(reader.u64());
      value = std::move(payload);
      break;
    }
    case 11: {
      RequestVerifiedPayload payload;
      payload.request = MitigationRequestId::from_value(reader.u64());
      RefToken ref;
      decode_ref(reader, ref);
      if (reader.ok() && ref.valid()) {
        payload.verification_ref = ExternalEvidenceRef{ref.text()};
      }
      value = std::move(payload);
      break;
    }
    case 12: {
      RequestFailedPayload payload;
      payload.request = MitigationRequestId::from_value(reader.u64());
      decode(reader, payload.failure);
      payload.transport_code = reader.i32();
      value = std::move(payload);
      break;
    }
    case 13: {
      RequestSupersededPayload payload;
      payload.request = MitigationRequestId::from_value(reader.u64());
      decode(reader, payload.reason);
      value = std::move(payload);
      break;
    }
    case 14: {
      RequestAbandonedPayload payload;
      payload.request = MitigationRequestId::from_value(reader.u64());
      value = std::move(payload);
      break;
    }
    case 15: {
      ConstraintRelaxedPayload payload;
      payload.obligation = ObligationId::from_value(reader.u64());
      decode(reader, payload.cls);
      RefToken authority;
      decode_ref(reader, authority);
      if (reader.ok() && authority.valid()) {
        payload.authority = ExternalAuthorityRef{authority.text()};
      }
      value = std::move(payload);
      break;
    }
    case 16: {
      ConstraintReimposedPayload payload;
      payload.obligation = ObligationId::from_value(reader.u64());
      value = std::move(payload);
      break;
    }
    case 17: {
      RecoveryBegunPayload payload;
      RefToken operator_ref;
      decode_ref(reader, operator_ref);
      if (reader.ok() && operator_ref.valid()) {
        payload.operator_ref = OperatorRef{operator_ref.text()};
      }
      value = std::move(payload);
      break;
    }
    case 18: {
      IncidentClosedPayload payload;
      RefToken operator_ref;
      decode_ref(reader, operator_ref);
      if (reader.ok() && operator_ref.valid()) {
        payload.operator_ref = OperatorRef{operator_ref.text()};
      }
      decode(reader, payload.disposition);
      value = std::move(payload);
      break;
    }
    case 19: {
      AuthorityRolledOverPayload payload;
      payload.incarnation = ControllerIncarnation::from_value(reader.u64());
      payload.epoch = ControlEpoch::from_value(reader.u64());
      value = std::move(payload);
      break;
    }
    case 20: {
      PolicyAdoptedPayload payload;
      decode(reader, payload.policy);
      decode(reader, payload.relaxation);
      value = std::move(payload);
      break;
    }
    case 21: {
      TargetBindingRegisteredPayload payload;
      RefToken zone;
      RefToken domain;
      decode_ref(reader, zone);
      decode_ref(reader, domain);
      if (!reader.ok()) {
        return;
      }
      if (zone.valid()) payload.zone = ThermalZoneRef{zone.text()};
      if (domain.valid()) payload.power_domain = PowerDomainRef{domain.text()};
      const std::uint32_t count = reader.u32();
      if (!reader.ok()) {
        return;
      }
      if (count > HardLimits::kMaxProbesPerZone) {
        reader.fail(Status::error(StatusCode::BoundsExceeded,
                                  "equipment list exceeds the structural bound"));
        return;
      }
      payload.equipment.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        RefToken equipment;
        decode_ref(reader, equipment);
        if (!reader.ok()) {
          return;
        }
        payload.equipment.push_back(EquipmentRef{equipment.text()});
      }
      value = std::move(payload);
      break;
    }
    case 22: {
      StoreRecoveredPayload payload;
      payload.rollover = reader.flag();
      payload.previous_incarnation = ControllerIncarnation::from_value(reader.u64());
      payload.previous_epoch = ControlEpoch::from_value(reader.u64());
      value = std::move(payload);
      break;
    }
    default:
      reader.fail(Status::error(StatusCode::UnknownEnumValue, "unknown journal payload kind"));
      return;
  }
}

void encode(Writer& writer, const JournalEntry& value) {
  writer.u64(value.sequence.value());
  encode(writer, value.at);
  writer.u64(value.epoch.value());
  writer.u64(value.incarnation.value());
  writer.u64(value.revision_before.value());
  writer.u64(value.revision_after.value());
  writer.u64(value.incident.value());
  encode(writer, value.kind);
  encode(writer, value.payload);
}

void decode(Reader& reader, JournalEntry& value) {
  value.sequence = JournalSequence::from_value(reader.u64());
  decode(reader, value.at);
  value.epoch = ControlEpoch::from_value(reader.u64());
  value.incarnation = ControllerIncarnation::from_value(reader.u64());
  value.revision_before = StateRevision::from_value(reader.u64());
  value.revision_after = StateRevision::from_value(reader.u64());
  value.incident = IncidentId::from_value(reader.u64());
  decode(reader, value.kind);
  decode(reader, value.payload);
  if (!reader.ok()) {
    return;
  }
  if (static_cast<std::uint8_t>(value.kind) != value.payload.index() + 1) {
    reader.fail(corrupt("journal entry kind does not match its payload"));
  }
}

}  // namespace summon::tem::codec

namespace summon::tem {

std::vector<std::uint8_t> encode_state(const DomainState& state) {
  codec::Writer writer;
  codec::encode(writer, state);
  return writer.bytes();
}

}  // namespace summon::tem
