// Thermal Emergency Manager -- mitigation request transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "tem/mitigation.hpp"
#include "tem/status.hpp"

namespace summon::tem {

// What the adjacent authority said when the request was handed to it. A
// transport answer is never evidence of the physical effect; it only records
// that the request was delivered and how it was received.
enum class DispatchOutcome : std::uint8_t {
  Accepted = 1,     // the adjacent authority accepted the request
  Refused = 2,      // the adjacent authority declined it
  Error = 3,        // delivery or processing failed
  Unsupported = 4,  // the adjacent authority cannot perform this class at all
};

[[nodiscard]] std::string_view dispatch_outcome_name(DispatchOutcome value) noexcept;
[[nodiscard]] std::optional<DispatchOutcome> dispatch_outcome_from_value(std::uint8_t value) noexcept;

// Recorded when no transport is configured: the request is issued into
// uncertainty and the escalation continues until verification arrives.
inline constexpr std::int32_t kNoTransportCode = -1;

struct DispatchResult {
  DispatchOutcome outcome{DispatchOutcome::Accepted};
  ExternalEvidenceRef external_ref{};
  bool has_external_ref{false};
  std::int32_t code{0};
  std::string detail{};

  [[nodiscard]] static DispatchResult accepted(const ExternalEvidenceRef& ref);
  [[nodiscard]] static DispatchResult accepted_without_ref();
  [[nodiscard]] static DispatchResult refused(std::string detail, std::int32_t code);
  [[nodiscard]] static DispatchResult error(std::string detail, std::int32_t code);
  [[nodiscard]] static DispatchResult unsupported(std::string detail);
};

// The interface an adjacent authority (or a synthetic stand-in) implements.
//
// Contract:
//   * Dispatch is called with no runtime lock held, and must not call back into
//     the runtime that called it;
//   * Dispatch must be safe to call concurrently;
//   * one call per attempt identity is a request to act. Re-dispatching the
//     same attempt identity is a defect: the runtime allocates a new attempt
//     id rather than retrying an indeterminate dispatch.
class IMitigationTransport {
 public:
  virtual ~IMitigationTransport() = default;
  [[nodiscard]] virtual DispatchResult Dispatch(const MitigationRequest& request) = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept { return "transport"; }
};

// A deterministic, in-process transport used by tests, the CLI, and the
// synthetic scenario engine. It records every dispatch it receives and answers
// according to a per-class rule. It is SYNTHETIC: it stands in for an adjacent
// authority that is not present in this environment.
class ScriptedTransport final : public IMitigationTransport {
 public:
  struct Rule {
    DispatchOutcome outcome{DispatchOutcome::Accepted};
    // Number of dispatches accepted before the outcome below takes effect.
    // Zero means the outcome applies immediately.
    std::uint32_t accept_first_n{0};
    std::int32_t code{0};
    std::string detail{};
    bool has_external_ref{true};
  };

  ScriptedTransport() = default;

  void SetRule(MitigationClass cls, Rule rule);
  void SetDefaultRule(Rule rule);
  [[nodiscard]] Rule rule_for(MitigationClass cls) const;

  [[nodiscard]] DispatchResult Dispatch(const MitigationRequest& request) override;
  [[nodiscard]] std::string_view name() const noexcept override { return "scripted-synthetic"; }

  [[nodiscard]] std::vector<MitigationRequest> dispatch_log() const;
  [[nodiscard]] std::size_t dispatch_count() const;
  [[nodiscard]] std::size_t dispatch_count(MitigationClass cls) const;
  void Reset();

 private:
  mutable std::mutex mutex_{};
  std::map<MitigationClass, Rule> rules_{};
  Rule default_rule_{};
  std::map<MitigationClass, std::uint32_t> accepted_{};
  std::vector<MitigationRequest> log_{};
};

}  // namespace summon::tem
