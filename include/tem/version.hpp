// Thermal Emergency Manager -- version and build identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <string_view>

#define TEM_VERSION_MAJOR 1
#define TEM_VERSION_MINOR 0
#define TEM_VERSION_PATCH 0

// On-disk store format version. Bumped only for incompatible layout changes.
#define TEM_STORE_FORMAT_VERSION 1u

namespace summon::tem {

inline constexpr std::uint32_t kVersionMajor = TEM_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = TEM_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = TEM_VERSION_PATCH;
inline constexpr std::uint32_t kStoreFormatVersion = TEM_STORE_FORMAT_VERSION;

// Human readable version string, e.g. "1.0.0".
[[nodiscard]] std::string_view version_string() noexcept;

// Version string plus format version, e.g. "1.0.0 (store format 1)".
[[nodiscard]] std::string_view version_banner() noexcept;

}  // namespace summon::tem
