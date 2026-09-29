// Thermal Emergency Manager -- unit and validation tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include <limits>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support.hpp"
#include "tem/codec.hpp"
#include "tem/plan.hpp"
#include "tem/transport.hpp"

using namespace summon::tem;
using namespace temtest;

TEM_TEST(units_checked_arithmetic_reports_overflow) {
  const auto max = std::numeric_limits<std::int32_t>::max();
  TEM_CHECK(!add_overflows<std::int32_t>(max, 0));
  TEM_CHECK(add_overflows<std::int32_t>(max, 1));
  TEM_CHECK(add_overflows<std::int32_t>(std::numeric_limits<std::int32_t>::min(), -1));
  TEM_CHECK(!add_overflows<std::int32_t>(1, 2));
  TEM_CHECK(!mul_overflows<std::int64_t>(0, max));
  TEM_CHECK(mul_overflows<std::int64_t>(std::numeric_limits<std::int64_t>::max(), 2));
  TEM_CHECK(!checked_add<std::int32_t>(1, 2).value() == false);
  TEM_CHECK(checked_add<std::int32_t>(max, 1).has_value() == false);
  TEM_CHECK(checked_sub<std::int32_t>(0, 1).value() == -1);
}

TEM_TEST(units_reject_out_of_range_quantities) {
  TEM_CHECK(MilliCelsius::try_make(250'000).has_value());
  TEM_CHECK(!MilliCelsius::try_make(250'001).has_value());
  TEM_CHECK(!MilliCelsius::try_make(-100'001).has_value());
  TEM_CHECK(BasisPoints::try_make(10'000).has_value());
  TEM_CHECK(!BasisPoints::try_make(10'001).has_value());
  const auto sum = MilliCelsius::from_value(250'000).checked_add(MilliCelsius::from_value(1));
  TEM_CHECK(!sum.has_value());
}

TEM_TEST(units_format_exactly) {
  TEM_CHECK_EQ(std::string("21.500 C"), format_temperature(MilliCelsius::from_value(21'500)));
  TEM_CHECK_EQ(std::string("-5.000 C"), format_temperature(MilliCelsius::from_value(-5'000)));
  TEM_CHECK_EQ(std::string("0.000 C"), format_temperature(MilliCelsius::from_value(0)));
  TEM_CHECK_EQ(std::string("3.000 C/min"), format_rate(MilliCelsiusPerMinute::from_value(3'000)));
  TEM_CHECK_EQ(std::string("50.00 %"), format_basis_points(BasisPoints::from_value(5'000)));
  TEM_CHECK_EQ(std::string("2.000 min"), format_duration(Duration::from_minutes(2)));
  TEM_CHECK_EQ(std::string("1.500 s"), format_duration(Duration::from_millis(1'500)));
  TEM_CHECK_EQ(std::string("200.000 ms"), format_duration(Duration::from_millis(200)));
  TEM_CHECK_EQ(std::string("1970-01-01T00:00:00.000Z"),
               format_timestamp(Timestamp::from_unix_nanos(0)));
  TEM_CHECK_EQ(std::string("2026-02-01T00:00:00.000Z"),
               format_timestamp(Timestamp::from_unix_nanos(1'769'904'000'000'000'000ll)));
}

TEM_TEST(ids_are_strongly_typed_and_ordered) {
  const IncidentId a = IncidentId::from_value(1);
  const IncidentId b = IncidentId::from_value(2);
  TEM_CHECK(a < b);
  TEM_CHECK(a.is_set());
  TEM_CHECK(IncidentId{}.is_absent());
  TEM_CHECK_EQ(std::uint64_t{2}, a.next().value());
  const Fingerprint fingerprint{0x0123456789abcdefull, 0xfedcba9876543210ull};
  TEM_CHECK_EQ(std::string("0123456789abcdeffedcba9876543210"), fingerprint.to_hex());
  TEM_CHECK(fingerprint.is_set());
  const IdempotencyKey key = IdempotencyKey::from_fingerprint(fingerprint);
  TEM_CHECK_EQ(fingerprint, key.as_fingerprint());
}

TEM_TEST(refs_validation_precedence_is_stable) {
  const auto empty = ThermalZoneRef::parse("");
  TEM_REQUIRE(!empty.ok());
  TEM_CHECK(empty.status().code() == StatusCode::EmptyField);

  const std::string too_long(200, 'a');
  const auto long_ref = ThermalZoneRef::parse(too_long);
  TEM_REQUIRE(!long_ref.ok());
  TEM_CHECK(long_ref.status().code() == StatusCode::FieldTooLong);

  const auto bad_char = ThermalZoneRef::parse("zone a");
  TEM_REQUIRE(!bad_char.ok());
  TEM_CHECK(bad_char.status().code() == StatusCode::InvalidCharacter);

  const auto traversal = ThermalZoneRef::parse("zone/../other");
  TEM_REQUIRE(!traversal.ok());
  TEM_CHECK(traversal.status().code() == StatusCode::InvalidCharacter);

  const auto slash = ThermalZoneRef::parse("/zone-a");
  TEM_REQUIRE(!slash.ok());
  TEM_CHECK(slash.status().code() == StatusCode::InvalidCharacter);

  const auto empty_segment = ThermalZoneRef::parse("zone-a//probe");
  TEM_REQUIRE(!empty_segment.ok());
  TEM_CHECK(empty_segment.status().code() == StatusCode::InvalidCharacter);

  const auto good = ThermalZoneRef::parse("zone-a/probe:1");
  TEM_REQUIRE(good.ok());
  TEM_CHECK_EQ(std::string("zone-a/probe:1"), good.value().text());

  // Kind confusion is impossible: the type carries the kind.
  TEM_CHECK(ThermalZoneRef::parse("x").value().kind() == RefKind::ThermalZone);
  TEM_CHECK(PowerDomainRef::parse("x").value().kind() == RefKind::PowerDomain);
}

TEM_TEST(status_code_names_are_stable_and_reversible) {
  TEM_CHECK_EQ(std::string("tem.stale_epoch"), std::string(status_code_name(StatusCode::StaleEpoch)));
  TEM_CHECK_EQ(std::string("tem.ok"), std::string(status_code_name(StatusCode::Ok)));
  const auto decoded = status_code_from_value(static_cast<std::uint16_t>(StatusCode::StoreLocked));
  TEM_REQUIRE(decoded.has_value());
  TEM_CHECK(*decoded == StatusCode::StoreLocked);
  TEM_CHECK(!status_code_from_value(65'000).has_value());
  const Status failure = Status::error(StatusCode::EvidenceStale, "reading aged out");
  TEM_CHECK(!failure.ok());
  TEM_CHECK_EQ(std::string("tem.evidence_stale: reading aged out"), failure.to_string());
}

TEM_TEST(policy_validation_rejects_unsafe_configurations) {
  ThermalPolicy policy;
  TEM_CHECK(validate_policy(policy).ok());

  ThermalPolicy inverted = policy;
  inverted.warning_enter = MilliCelsius::from_value(20'000);
  auto result = validate_policy(inverted);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.status().code() == StatusCode::ValueOutOfRange);

  ThermalPolicy no_hysteresis = policy;
  no_hysteresis.recovery_margin = MilliCelsius::from_value(0);
  result = validate_policy(no_hysteresis);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.status().code() == StatusCode::ValueOutOfRange);

  ThermalPolicy wide_band = policy;
  wide_band.recovery_margin = MilliCelsius::from_value(7'000);
  result = validate_policy(wide_band);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.status().code() == StatusCode::ValueOutOfRange);

  ThermalPolicy zero_dwell = policy;
  zero_dwell.recovery_dwell = Duration::from_seconds(0);
  result = validate_policy(zero_dwell);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.status().code() == StatusCode::ValueOutOfRange);

  ThermalPolicy huge_bounds = policy;
  huge_bounds.bounds.max_zones = 100'000;
  result = validate_policy(huge_bounds);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.status().code() == StatusCode::BoundsExceeded);
}

TEM_TEST(policy_thresholds_are_exact) {
  ThermalPolicy policy;
  TEM_CHECK_EQ(std::int32_t{38'000},
               severity_enter_threshold(Severity::Critical, policy).value());
  const auto recovery = severity_recovery_threshold(Severity::Critical, policy);
  TEM_REQUIRE(recovery.ok());
  TEM_CHECK_EQ(std::int32_t{36'000}, recovery.value().value());
  const auto nominal = severity_recovery_threshold(Severity::Nominal, policy);
  TEM_REQUIRE(nominal.ok());
  TEM_CHECK_EQ(std::int32_t{0}, nominal.value().value());
}

TEM_TEST(obligation_validation_and_relaxation_scope) {
  Bounds bounds;
  ProtectedObligation obligation;
  obligation.ref = "obligation/interlock-1";
  obligation.protection = ProtectionClass::HardSafetyInterlock;
  obligation.forbidden_classes = mitigation_class_bit(MitigationClass::ReducePower);
  Status result = validate_obligation(obligation, bounds);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.code() == StatusCode::InvalidArgument);

  obligation.authority = ExternalAuthorityRef{std::string("ops/safety")};
  TEM_CHECK(validate_obligation(obligation, bounds).ok());

  ProtectedObligation empty_mask = obligation;
  empty_mask.forbidden_classes = 0;
  result = validate_obligation(empty_mask, bounds);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.code() == StatusCode::ValueOutOfRange);

  ProtectedObligation bad_bits = obligation;
  bad_bits.forbidden_classes = 0x80;
  result = validate_obligation(bad_bits, bounds);
  TEM_REQUIRE(!result.ok());
  TEM_CHECK(result.code() == StatusCode::ValueOutOfRange);

  RelaxationPolicy relaxation;
  relaxation.relaxable_classes = 0xFF;
  TEM_CHECK(!relaxation_allowed(relaxation, ProtectionClass::HardSafetyInterlock,
                                MitigationClass::ReducePower));
  TEM_CHECK(!relaxation_allowed(relaxation, ProtectionClass::Regulatory,
                                MitigationClass::ReducePower));
  TEM_CHECK(relaxation_allowed(relaxation, ProtectionClass::AdvisoryOptimization,
                               MitigationClass::ReducePower));

  RelaxationPolicy disabled;
  disabled.allow_advisory_relaxation = false;
  TEM_CHECK(!relaxation_allowed(disabled, ProtectionClass::AdvisoryOptimization,
                                MitigationClass::ReducePower));
}

TEM_TEST(fingerprint_is_semantic_and_epoch_sensitive) {
  const auto zone = ThermalZoneRef::parse("zone-a").value();
  const auto domain = PowerDomainRef::parse("power/a").value();
  const RequestTarget zone_target = RequestTarget::for_zone(zone);
  const RequestTarget domain_target = RequestTarget::for_power_domain(domain);

  const Fingerprint base =
      fingerprint_of_request_fields(IncidentId::from_value(1), IncidentGeneration::from_value(1),
                                    ControlEpoch::from_value(1), MitigationClass::DerateAccelerators,
                                    zone_target, BasisPoints::from_value(3'000));
  const Fingerprint same =
      fingerprint_of_request_fields(IncidentId::from_value(1), IncidentGeneration::from_value(1),
                                    ControlEpoch::from_value(1), MitigationClass::DerateAccelerators,
                                    zone_target, BasisPoints::from_value(3'000));
  TEM_CHECK_EQ(base, same);

  const Fingerprint other_epoch =
      fingerprint_of_request_fields(IncidentId::from_value(1), IncidentGeneration::from_value(1),
                                    ControlEpoch::from_value(2), MitigationClass::DerateAccelerators,
                                    zone_target, BasisPoints::from_value(3'000));
  TEM_CHECK(!(base == other_epoch));

  const Fingerprint other_intensity =
      fingerprint_of_request_fields(IncidentId::from_value(1), IncidentGeneration::from_value(1),
                                    ControlEpoch::from_value(1), MitigationClass::DerateAccelerators,
                                    zone_target, BasisPoints::from_value(5'000));
  TEM_CHECK(!(base == other_intensity));

  const Fingerprint other_target =
      fingerprint_of_request_fields(IncidentId::from_value(1), IncidentGeneration::from_value(1),
                                    ControlEpoch::from_value(1), MitigationClass::DerateAccelerators,
                                    domain_target, BasisPoints::from_value(3'000));
  TEM_CHECK(!(base == other_target));
}

TEM_TEST(codec_round_trip_and_truncation_detection) {
  codec::Writer writer;
  writer.u8(0x12);
  writer.u16(0x3456);
  writer.u32(0x789abcde);
  writer.u64(0x0123456789abcdefull);
  writer.i32(-17);
  writer.i64(-1'234'567'890'123ll);
  writer.flag(true);
  TEM_CHECK(writer.text(std::string("hello"), 16));
  TEM_REQUIRE(writer.ok());

  codec::Reader reader(writer.bytes());
  TEM_CHECK_EQ(std::uint8_t{0x12}, reader.u8());
  TEM_CHECK_EQ(std::uint16_t{0x3456}, reader.u16());
  TEM_CHECK_EQ(std::uint32_t{0x789abcde}, reader.u32());
  TEM_CHECK_EQ(std::uint64_t{0x0123456789abcdefull}, reader.u64());
  TEM_CHECK_EQ(std::int32_t{-17}, reader.i32());
  TEM_CHECK_EQ(std::int64_t{-1'234'567'890'123ll}, reader.i64());
  TEM_CHECK(reader.flag());
  std::string text;
  TEM_CHECK(reader.text(text, 16));
  TEM_CHECK_EQ(std::string("hello"), text);
  TEM_CHECK(reader.ok());
  TEM_CHECK(reader.at_end());

  const std::vector<std::uint8_t> truncated(writer.bytes().begin(), writer.bytes().end() - 3);
  codec::Reader short_reader(truncated);
  (void)short_reader.u8();
  (void)short_reader.u16();
  (void)short_reader.u32();
  (void)short_reader.u64();
  (void)short_reader.i32();
  (void)short_reader.i64();
  (void)short_reader.flag();
  std::string other;
  (void)short_reader.text(other, 16);
  TEM_CHECK(!short_reader.ok());
  TEM_CHECK(short_reader.error().code() == StatusCode::StoreTruncated);

  codec::Writer bounded;
  TEM_CHECK(!bounded.text(std::string("0123456789"), 4));
  TEM_CHECK(!bounded.ok());
  TEM_CHECK(bounded.error().code() == StatusCode::FieldTooLong);
}

TEM_TEST(codec_checksums_are_known_vectors) {
  const std::string check = "123456789";
  const auto bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(check.data()), check.size());
  TEM_CHECK_EQ(std::uint32_t{0xE3069283}, codec::crc32c(bytes));
  TEM_CHECK_EQ(std::uint32_t{0}, codec::crc32c(std::span<const std::uint8_t>()));
  const Fingerprint first = codec::fnv1a128(bytes);
  const Fingerprint second = codec::fnv1a128(bytes);
  TEM_CHECK_EQ(first, second);
  const std::string other = "123456780";
  const Fingerprint third = codec::fnv1a128(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(other.data()), other.size()));
  TEM_CHECK(!(first == third));
}

TEM_TEST(scripted_transport_is_deterministic_and_bounded) {
  ScriptedTransport transport;
  ScriptedTransport::Rule rule;
  rule.accept_first_n = 1;
  rule.outcome = DispatchOutcome::Refused;
  rule.code = 42;
  rule.detail = "no";
  transport.SetDefaultRule(rule);

  MitigationRequest request;
  request.cls = MitigationClass::DerateAccelerators;
  request.attempt = AttemptId::from_value(7);
  request.epoch = ControlEpoch::from_value(1);
  request.incident = IncidentId::from_value(1);
  request.incident_generation = IncidentGeneration::from_value(1);
  const DispatchResult first = transport.Dispatch(request);
  TEM_CHECK(first.outcome == DispatchOutcome::Accepted);
  TEM_REQUIRE(first.has_external_ref);
  TEM_CHECK_EQ(std::string("dispatch/derate-accelerators/7"), first.external_ref.text());
  const DispatchResult second = transport.Dispatch(request);
  TEM_CHECK(second.outcome == DispatchOutcome::Refused);
  TEM_CHECK_EQ(std::int32_t{42}, second.code);
  TEM_CHECK_EQ(std::size_t{2}, transport.dispatch_count());
  TEM_CHECK_EQ(std::size_t{2}, transport.dispatch_count(MitigationClass::DerateAccelerators));
  TEM_CHECK_EQ(std::size_t{0}, transport.dispatch_count(MitigationClass::DrainWorkload));
}

TEM_TEST(severity_and_lifecycle_helpers_are_total) {
  for (std::uint8_t i = 0; i < kSeverityCount; ++i) {
    const auto value = severity_from_value(i);
    TEM_REQUIRE(value.has_value());
    TEM_CHECK(severity_ordinal(*value) == i);
  }
  TEM_CHECK(!severity_from_value(kSeverityCount).has_value());
  TEM_CHECK(!lifecycle_from_value(99).has_value());
  TEM_CHECK(lifecycle_transition_allowed(Lifecycle::None, Lifecycle::Active));
  TEM_CHECK(!lifecycle_transition_allowed(Lifecycle::None, Lifecycle::Recovered));
  TEM_CHECK(!lifecycle_transition_allowed(Lifecycle::Active, Lifecycle::Recovered));
  TEM_CHECK(lifecycle_transition_allowed(Lifecycle::Recovering, Lifecycle::Recovered));
  TEM_CHECK(!lifecycle_transition_allowed(Lifecycle::Closed, Lifecycle::Active));
  TEM_CHECK(lifecycle_transition_allowed(Lifecycle::Recovered, Lifecycle::Active));
}

TEM_TEST(mitigation_class_requirements_ladder) {
  TEM_CHECK_EQ(std::uint8_t{0}, required_class_mask(Severity::Nominal));
  TEM_CHECK_EQ(std::uint8_t{0}, required_class_mask(Severity::Advisory));
  TEM_CHECK((required_class_mask(Severity::Warning) & mitigation_class_bit(MitigationClass::DerateAccelerators)) != 0);
  TEM_CHECK((required_class_mask(Severity::Critical) & mitigation_class_bit(MitigationClass::DrainWorkload)) != 0);
  TEM_CHECK((required_class_mask(Severity::Emergency) & mitigation_class_bit(MitigationClass::ReducePower)) != 0);
  TEM_CHECK((required_class_mask(Severity::Catastrophic) & mitigation_class_bit(MitigationClass::IsolateEquipment)) != 0);
  TEM_CHECK_EQ(static_cast<int>(Severity::Warning),
               static_cast<int>(mitigation_class_min_severity(MitigationClass::DerateAccelerators)));
  TEM_CHECK_EQ(static_cast<int>(Severity::Catastrophic),
               static_cast<int>(mitigation_class_min_severity(MitigationClass::IsolateEquipment)));
  TEM_CHECK_EQ(std::uint16_t{9'000},
               planned_intensity(MitigationClass::DerateAccelerators, Severity::Catastrophic).value());
  TEM_CHECK_EQ(std::uint16_t{0},
               planned_intensity(MitigationClass::DerateAccelerators, Severity::Nominal).value());
}

TEM_TEST_MAIN()
