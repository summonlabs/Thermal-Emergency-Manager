// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/units.hpp"

#include <cstdio>
#include <cstdint>
#include <string>

namespace summon::tem {
namespace {

// Formats value/1000 with exactly three decimal places and an explicit sign.
std::string format_milli(std::int64_t value, const char* unit, int decimals) {
  const bool negative = value < 0;
  const std::uint64_t magnitude = negative ? static_cast<std::uint64_t>(-(value + 1)) + 1u
                                           : static_cast<std::uint64_t>(value);
  std::uint64_t scale = 1;
  for (int i = 0; i < decimals; ++i) {
    scale *= 10u;
  }
  const std::uint64_t whole = magnitude / scale;
  const std::uint64_t fraction = magnitude % scale;
  char buffer[64];
  if (decimals == 0) {
    std::snprintf(buffer, sizeof(buffer), "%s%llu %s", negative ? "-" : "",
                  static_cast<unsigned long long>(whole), unit);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%s%llu.%0*llu %s", negative ? "-" : "",
                  static_cast<unsigned long long>(whole), decimals,
                  static_cast<unsigned long long>(fraction), unit);
  }
  return std::string(buffer);
}

std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) {
  const std::int64_t quotient = numerator / denominator;
  const std::int64_t remainder = numerator % denominator;
  if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
    return quotient - 1;
  }
  return quotient;
}

}  // namespace

std::int64_t Timestamp::seconds_until(Timestamp later) const noexcept {
  return floor_div(later.unix_nanos() - nanos_, 1'000'000'000);
}

std::string format_temperature(MilliCelsius value) {
  return format_milli(value.value(), "C", 3);
}

std::string format_rate(MilliCelsiusPerMinute value) {
  return format_milli(value.value(), "C/min", 3);
}

std::string format_basis_points(BasisPoints value) {
  return format_milli(value.value(), "%", 2);
}

std::string format_duration(Duration value) {
  const std::int64_t nanos = value.nanos();
  const bool negative = nanos < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(nanos + 1)) + 1u : static_cast<std::uint64_t>(nanos);
  char buffer[64];
  if (magnitude >= 60'000'000'000ull) {
    const std::uint64_t whole = magnitude / 60'000'000'000ull;
    const std::uint64_t fraction = (magnitude % 60'000'000'000ull) / 1'000'000ull;
    std::snprintf(buffer, sizeof(buffer), "%s%llu.%03llu min", negative ? "-" : "",
                  static_cast<unsigned long long>(whole),
                  static_cast<unsigned long long>(fraction));
  } else if (magnitude >= 1'000'000'000ull) {
    const std::uint64_t whole = magnitude / 1'000'000'000ull;
    const std::uint64_t fraction = (magnitude % 1'000'000'000ull) / 1'000'000ull;
    std::snprintf(buffer, sizeof(buffer), "%s%llu.%03llu s", negative ? "-" : "",
                  static_cast<unsigned long long>(whole),
                  static_cast<unsigned long long>(fraction));
  } else {
    const std::uint64_t whole = magnitude / 1'000'000ull;
    const std::uint64_t fraction = (magnitude % 1'000'000ull) / 1'000ull;
    std::snprintf(buffer, sizeof(buffer), "%s%llu.%03llu ms", negative ? "-" : "",
                  static_cast<unsigned long long>(whole),
                  static_cast<unsigned long long>(fraction));
  }
  return std::string(buffer);
}

std::string format_timestamp(Timestamp value) {
  const std::int64_t nanos = value.unix_nanos();
  const std::int64_t seconds = floor_div(nanos, 1'000'000'000);
  std::int64_t millis = nanos % 1'000'000'000;
  if (millis < 0) {
    millis += 1'000'000'000;
  }
  const std::int64_t days = floor_div(seconds, 86'400);
  const std::int64_t second_of_day = seconds - days * 86'400;

  // Civil-from-days (Howard Hinnant's algorithm), valid for the full i64 range.
  const std::int64_t z = days + 719'468;
  const std::int64_t era = floor_div(z, 146'097);
  const std::int64_t doe = z - era * 146'097;
  const std::int64_t yoe = (doe - doe / 1'460 + doe / 36'524 - doe / 146'096) / 365;
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = m <= 2 ? y + 1 : y;

  const std::int64_t hour = second_of_day / 3'600;
  const std::int64_t minute = (second_of_day % 3'600) / 60;
  const std::int64_t second = second_of_day % 60;

  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ",
                static_cast<long long>(year), static_cast<long long>(m), static_cast<long long>(d),
                static_cast<long long>(hour), static_cast<long long>(minute),
                static_cast<long long>(second), static_cast<long long>(millis / 1'000'000));
  return std::string(buffer);
}

}  // namespace summon::tem
