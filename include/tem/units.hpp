// Thermal Emergency Manager -- exact integer quantities with explicit units.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace summon::tem {

// ---------------------------------------------------------------------------
// Checked integer arithmetic.
//
// Every physical computation in this runtime is exact integer arithmetic over
// a checked helper. Floating point is never an authority or accounting
// boundary. Overflow is reported, never wrapped.
// ---------------------------------------------------------------------------

template <class T>
[[nodiscard]] constexpr bool add_overflows(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral required");
  if constexpr (std::is_unsigned_v<T>) {
    return a > static_cast<T>(std::numeric_limits<T>::max() - b);
  } else {
    if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return true;
    }
    if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) {
      return true;
    }
    return false;
  }
}

template <class T>
[[nodiscard]] constexpr bool sub_overflows(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral required");
  if constexpr (std::is_unsigned_v<T>) {
    return a < b;
  } else {
    if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) {
      return true;
    }
    if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) {
      return true;
    }
    return false;
  }
}

// Overflow check for multiplication. Signed magnitudes are computed in the
// corresponding unsigned type, so no intermediate result can itself overflow.
template <class T>
[[nodiscard]] constexpr bool mul_overflows(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral required");
  if (a == 0 || b == 0) {
    return false;
  }
  const T max = std::numeric_limits<T>::max();
  if constexpr (std::is_unsigned_v<T>) {
    return a > static_cast<T>(max / b);
  } else {
    using U = std::make_unsigned_t<T>;
    const bool negative = (a < 0) != (b < 0);
    const U magnitude_a = a < 0 ? static_cast<U>(0) - static_cast<U>(a) : static_cast<U>(a);
    const U magnitude_b = b < 0 ? static_cast<U>(0) - static_cast<U>(b) : static_cast<U>(b);
    const U limit = negative ? static_cast<U>(max) + U{1} : static_cast<U>(max);
    return magnitude_a > limit / magnitude_b;
  }
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_add(T a, T b) noexcept {
  if (add_overflows(a, b)) {
    return std::nullopt;
  }
  return static_cast<T>(a + b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  if (sub_overflows(a, b)) {
    return std::nullopt;
  }
  return static_cast<T>(a - b);
}

template <class T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  if (mul_overflows(a, b)) {
    return std::nullopt;
  }
  return static_cast<T>(a * b);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

// A wall-clock instant expressed as nanoseconds since the Unix epoch (UTC).
// 0 is the epoch itself, not "absent"; absence is modelled with std::optional.
class Timestamp {
 public:
  // Timestamp{} is the epoch itself and Timestamp::from_unix_nanos(n) is an
  // exact instant. The single-argument constructor is private, so an integer
  // never converts to a timestamp implicitly.
  constexpr Timestamp() noexcept = default;
  [[nodiscard]] static constexpr Timestamp from_unix_nanos(std::int64_t nanos) noexcept {
    return Timestamp{nanos};
  }
  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return nanos_; }

  [[nodiscard]] std::optional<Timestamp> checked_add(class Duration delta) const noexcept;
  [[nodiscard]] std::int64_t seconds_until(Timestamp later) const noexcept;

  [[nodiscard]] friend constexpr bool operator==(Timestamp, Timestamp) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;

 private:
  constexpr explicit Timestamp(std::int64_t nanos) noexcept : nanos_(nanos) {}
  std::int64_t nanos_{0};
};

// A signed elapsed duration in nanoseconds. Signed because the difference of
// two timestamps may be negative when evidence arrives out of order; such a
// difference is reported explicitly rather than clamped to zero.
class Duration {
 public:
  constexpr Duration() noexcept = default;
  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t nanos) noexcept {
    return Duration{nanos};
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t millis) noexcept {
    return Duration{millis * 1'000'000};
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t seconds) noexcept {
    return Duration{seconds * 1'000'000'000};
  }
  [[nodiscard]] static constexpr Duration from_minutes(std::int64_t minutes) noexcept {
    return Duration{minutes * 60'000'000'000};
  }
  [[nodiscard]] static constexpr Duration from_hours(std::int64_t hours) noexcept {
    return Duration{hours * 3'600'000'000'000};
  }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return nanos_ / 1'000'000; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_positive() const noexcept { return nanos_ > 0; }

  [[nodiscard]] std::optional<Duration> checked_add(Duration other) const noexcept {
    const auto sum = tem::checked_add(nanos_, other.nanos_);
    if (!sum.has_value()) {
      return std::nullopt;
    }
    return Duration{*sum};
  }
  [[nodiscard]] std::optional<Duration> checked_sub(Duration other) const noexcept {
    const auto diff = tem::checked_sub(nanos_, other.nanos_);
    if (!diff.has_value()) {
      return std::nullopt;
    }
    return Duration{*diff};
  }

  [[nodiscard]] friend constexpr bool operator==(Duration, Duration) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Duration, Duration) noexcept = default;

 private:
  constexpr explicit Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}
  std::int64_t nanos_{0};
};

inline std::optional<Timestamp> Timestamp::checked_add(Duration delta) const noexcept {
  const auto sum = tem::checked_add(nanos_, delta.nanos());
  if (!sum.has_value()) {
    return std::nullopt;
  }
  return Timestamp{*sum};
}

// Difference in seconds (floor), saturating is not performed: the exact
// nanosecond difference is available through Duration arithmetic.
inline std::int64_t timestamp_difference_nanos(Timestamp earlier, Timestamp later) noexcept {
  return later.unix_nanos() - earlier.unix_nanos();
}

// ---------------------------------------------------------------------------
// Physical quantities
// ---------------------------------------------------------------------------

// Quantity tags give every physical dimension its own type and admissible
// range. The range is enforced during decoding and construction from external
// input; internal arithmetic uses checked_add and reports overflow instead of
// saturating silently.
struct MilliCelsiusTag {
  using rep = std::int32_t;
  static constexpr rep kMin = -100'000;  // -100.000 C
  static constexpr rep kMax = 250'000;   // +250.000 C
  static constexpr const char* kUnit = "mC";
};
struct MilliCelsiusPerMinuteTag {
  using rep = std::int32_t;
  static constexpr rep kMin = -600'000;  // -600.000 C/min
  static constexpr rep kMax = 600'000;   // +600.000 C/min
  static constexpr const char* kUnit = "mC/min";
};
struct WattsTag {
  using rep = std::int64_t;
  static constexpr rep kMin = 0;
  static constexpr rep kMax = 1'000'000'000'000;  // 1 TW
  static constexpr const char* kUnit = "W";
};
struct BasisPointsTag {
  using rep = std::uint16_t;
  static constexpr rep kMin = 0;
  static constexpr rep kMax = 10'000;  // 100.00 %
  static constexpr const char* kUnit = "bp";
};

template <class Tag>
class Quantity {
 public:
  using rep = typename Tag::rep;

  constexpr Quantity() noexcept = default;
  [[nodiscard]] static constexpr Quantity from_value(rep value) noexcept { return Quantity{value}; }

  [[nodiscard]] static constexpr bool in_range(rep value) noexcept {
    return value >= Tag::kMin && value <= Tag::kMax;
  }

  [[nodiscard]] static constexpr std::optional<Quantity> try_make(rep value) noexcept {
    if (!in_range(value)) {
      return std::nullopt;
    }
    return Quantity{value};
  }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  [[nodiscard]] std::optional<Quantity> checked_add(Quantity other) const noexcept {
    const auto sum = tem::checked_add(value_, other.value_);
    if (!sum.has_value()) {
      return std::nullopt;
    }
    return try_make(*sum);
  }
  [[nodiscard]] std::optional<Quantity> checked_sub(Quantity other) const noexcept {
    const auto diff = tem::checked_sub(value_, other.value_);
    if (!diff.has_value()) {
      return std::nullopt;
    }
    return try_make(*diff);
  }

  [[nodiscard]] friend constexpr bool operator==(Quantity, Quantity) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;

 private:
  constexpr explicit Quantity(rep value) noexcept : value_(value) {}
  rep value_{0};
};

using MilliCelsius = Quantity<MilliCelsiusTag>;
using MilliCelsiusPerMinute = Quantity<MilliCelsiusPerMinuteTag>;
using Watts = Quantity<WattsTag>;
using BasisPoints = Quantity<BasisPointsTag>;

// ---------------------------------------------------------------------------
// Formatting helpers (diagnostics and audit text only; never parsed back into
// authority-bearing state).
// ---------------------------------------------------------------------------

// "21.500 C"
[[nodiscard]] std::string format_temperature(MilliCelsius value);
// "3.250 C/min"
[[nodiscard]] std::string format_rate(MilliCelsiusPerMinute value);
// "42.50 %"
[[nodiscard]] std::string format_basis_points(BasisPoints value);
// "1.500 s", "3.000 min", "250.000 ms"
[[nodiscard]] std::string format_duration(Duration value);
// RFC 3339 UTC with millisecond precision, e.g. "2026-02-01T12:00:00.000Z"
[[nodiscard]] std::string format_timestamp(Timestamp value);

}  // namespace summon::tem
