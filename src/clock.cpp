// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/clock.hpp"

#include <chrono>

namespace summon::tem {

Timestamp SystemClock::Now() const noexcept {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  return Timestamp::from_unix_nanos(static_cast<std::int64_t>(nanos));
}

}  // namespace summon::tem
