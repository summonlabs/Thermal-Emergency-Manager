// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/version.hpp"

#include <string>

namespace summon::tem {
namespace {
constexpr const char* kVersion = "1.0.0";
constexpr const char* kBanner = "1.0.0 (store format " "1" ")";
}  // namespace

std::string_view version_string() noexcept { return kVersion; }
std::string_view version_banner() noexcept { return kBanner; }

}  // namespace summon::tem
