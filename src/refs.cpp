// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/refs.hpp"

#include <string>
#include <string_view>

namespace summon::tem {
namespace {

bool is_allowed_ref_char(char c) noexcept {
  const unsigned char uc = static_cast<unsigned char>(c);
  if (uc >= 'A' && uc <= 'Z') return true;
  if (uc >= 'a' && uc <= 'z') return true;
  if (uc >= '0' && uc <= '9') return true;
  switch (c) {
    case '.': case '_': case ':': case '+': case '-': case '/':
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string_view ref_kind_name(RefKind kind) noexcept {
  switch (kind) {
    case RefKind::ThermalZone: return "thermal-zone";
    case RefKind::ThermalProbe: return "thermal-probe";
    case RefKind::Workload: return "workload";
    case RefKind::PowerDomain: return "power-domain";
    case RefKind::Equipment: return "equipment";
    case RefKind::CoolingDomain: return "cooling-domain";
    case RefKind::Operator: return "operator";
    case RefKind::ExternalAuthority: return "external-authority";
    case RefKind::ExternalEvidence: return "external-evidence";
    case RefKind::SensorSource: return "sensor-source";
  }
  return "unknown";
}

std::optional<RefKind> ref_kind_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return RefKind::ThermalZone;
    case 2: return RefKind::ThermalProbe;
    case 3: return RefKind::Workload;
    case 4: return RefKind::PowerDomain;
    case 5: return RefKind::Equipment;
    case 6: return RefKind::CoolingDomain;
    case 7: return RefKind::Operator;
    case 8: return RefKind::ExternalAuthority;
    case 9: return RefKind::ExternalEvidence;
    case 10: return RefKind::SensorSource;
    default: return std::nullopt;
  }
}

Result<std::string> canonical_ref_text(RefKind kind, std::string_view text) {
  const std::string context(ref_kind_name(kind));
  if (text.empty()) {
    return Status::error(StatusCode::EmptyField, "reference token is empty").with_context(context);
  }
  if (text.size() > kMaxRefLength) {
    return Status::error(StatusCode::FieldTooLong, "reference token exceeds the maximum length")
        .with_context(context);
  }
  for (const char c : text) {
    if (!is_allowed_ref_char(c)) {
      return Status::error(StatusCode::InvalidCharacter,
                           "reference token contains a character outside [A-Za-z0-9._:+-/]")
          .with_context(context);
    }
  }
  if (text.front() == '/' || text.back() == '/') {
    return Status::error(StatusCode::InvalidCharacter,
                         "reference token must not start or end with a separator")
        .with_context(context);
  }
  std::size_t segment_start = 0;
  while (segment_start <= text.size()) {
    const std::size_t separator = text.find('/', segment_start);
    const std::size_t segment_end = separator == std::string_view::npos ? text.size() : separator;
    const std::string_view segment = text.substr(segment_start, segment_end - segment_start);
    if (segment.empty()) {
      return Status::error(StatusCode::InvalidCharacter,
                           "reference token must not contain an empty segment")
          .with_context(context);
    }
    if (segment == "." || segment == "..") {
      return Status::error(StatusCode::InvalidCharacter,
                           "reference token must not contain a relative path segment")
          .with_context(context);
    }
    if (separator == std::string_view::npos) {
      break;
    }
    segment_start = separator + 1;
  }
  return std::string(text);
}

Result<RefToken> RefToken::parse(RefKind kind, std::string_view text) {
  auto canonical = canonical_ref_text(kind, text);
  if (!canonical.ok()) {
    return canonical.status();
  }
  return RefToken{kind, std::move(canonical).value()};
}

}  // namespace summon::tem
