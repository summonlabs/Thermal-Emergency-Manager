// Thermal Emergency Manager -- injectable clock.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>

#include "tem/units.hpp"

namespace summon::tem {

// The runtime never reads the wall clock on its own: every operation that
// depends on time takes an explicit instant, and the clock interface exists
// only so callers can obtain one. This keeps deterministic replay possible and
// makes freshness rules testable without sleeping.
class IClock {
 public:
  virtual ~IClock() = default;
  [[nodiscard]] virtual Timestamp Now() const noexcept = 0;
};

// Reads the host clock. The only place in the runtime that touches system time.
class SystemClock final : public IClock {
 public:
  [[nodiscard]] Timestamp Now() const noexcept override;
};

// A clock the caller controls explicitly.
class ManualClock final : public IClock {
 public:
  ManualClock() noexcept = default;
  explicit ManualClock(Timestamp start) noexcept : now_(start) {}

  [[nodiscard]] Timestamp Now() const noexcept override { return now_; }

  void Set(Timestamp value) noexcept { now_ = value; }
  void Advance(Duration delta) noexcept { now_ = now_.checked_add(delta).value_or(now_); }

 private:
  Timestamp now_{};
};

}  // namespace summon::tem
