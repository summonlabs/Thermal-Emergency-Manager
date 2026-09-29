// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/transport.hpp"

#include <string>

namespace summon::tem {

std::string_view dispatch_outcome_name(DispatchOutcome value) noexcept {
  switch (value) {
    case DispatchOutcome::Accepted: return "accepted";
    case DispatchOutcome::Refused: return "refused";
    case DispatchOutcome::Error: return "error";
    case DispatchOutcome::Unsupported: return "unsupported";
  }
  return "unknown";
}

std::optional<DispatchOutcome> dispatch_outcome_from_value(std::uint8_t value) noexcept {
  switch (value) {
    case 1: return DispatchOutcome::Accepted;
    case 2: return DispatchOutcome::Refused;
    case 3: return DispatchOutcome::Error;
    case 4: return DispatchOutcome::Unsupported;
    default: return std::nullopt;
  }
}

DispatchResult DispatchResult::accepted(const ExternalEvidenceRef& ref) {
  DispatchResult result;
  result.outcome = DispatchOutcome::Accepted;
  result.external_ref = ref;
  result.has_external_ref = ref.valid();
  return result;
}

DispatchResult DispatchResult::accepted_without_ref() {
  DispatchResult result;
  result.outcome = DispatchOutcome::Accepted;
  return result;
}

DispatchResult DispatchResult::refused(std::string detail, std::int32_t code) {
  DispatchResult result;
  result.outcome = DispatchOutcome::Refused;
  result.detail = std::move(detail);
  result.code = code;
  return result;
}

DispatchResult DispatchResult::error(std::string detail, std::int32_t code) {
  DispatchResult result;
  result.outcome = DispatchOutcome::Error;
  result.detail = std::move(detail);
  result.code = code;
  return result;
}

DispatchResult DispatchResult::unsupported(std::string detail) {
  DispatchResult result;
  result.outcome = DispatchOutcome::Unsupported;
  result.detail = std::move(detail);
  result.code = 0;
  return result;
}

void ScriptedTransport::SetRule(MitigationClass cls, Rule rule) {
  std::lock_guard<std::mutex> guard(mutex_);
  rules_[cls] = std::move(rule);
}

void ScriptedTransport::SetDefaultRule(Rule rule) {
  std::lock_guard<std::mutex> guard(mutex_);
  default_rule_ = std::move(rule);
}

ScriptedTransport::Rule ScriptedTransport::rule_for(MitigationClass cls) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = rules_.find(cls);
  return it == rules_.end() ? default_rule_ : it->second;
}

DispatchResult ScriptedTransport::Dispatch(const MitigationRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  log_.push_back(request);
  const auto it = rules_.find(request.cls);
  const Rule& rule = it == rules_.end() ? default_rule_ : it->second;

  ExternalEvidenceRef reference;
  if (rule.has_external_ref) {
    const std::string text = std::string("dispatch/") +
                             std::string(mitigation_class_name(request.cls)) + "/" +
                             std::to_string(request.attempt.value());
    auto parsed = ExternalEvidenceRef::parse(text);
    if (parsed.ok()) {
      reference = std::move(parsed).value();
    }
  }

  const std::uint32_t accepted = accepted_[request.cls];
  if (accepted < rule.accept_first_n) {
    accepted_[request.cls] = accepted + 1;
    return DispatchResult::accepted(reference);
  }
  switch (rule.outcome) {
    case DispatchOutcome::Accepted:
      return DispatchResult::accepted(reference);
    case DispatchOutcome::Refused:
      return DispatchResult::refused(rule.detail, rule.code);
    case DispatchOutcome::Error:
      return DispatchResult::error(rule.detail, rule.code);
    case DispatchOutcome::Unsupported:
      return DispatchResult::unsupported(rule.detail);
  }
  return DispatchResult::error("unknown scripted outcome", -2);
}

std::vector<MitigationRequest> ScriptedTransport::dispatch_log() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return log_;
}

std::size_t ScriptedTransport::dispatch_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return log_.size();
}

std::size_t ScriptedTransport::dispatch_count(MitigationClass cls) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t count = 0;
  for (const MitigationRequest& request : log_) {
    if (request.cls == cls) {
      ++count;
    }
  }
  return count;
}

void ScriptedTransport::Reset() {
  std::lock_guard<std::mutex> guard(mutex_);
  log_.clear();
  accepted_.clear();
}

}  // namespace summon::tem
