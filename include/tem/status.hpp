// Thermal Emergency Manager -- public status/error contract.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace summon::tem {

// Machine-readable outcome codes.
//
// Codes are grouped by domain and the numeric value is part of the public
// machine contract: a given code never changes value between compatible
// releases. Validation precedence is documented per public operation and is
// stable, so the same invalid request always produces the same primary code.
enum class StatusCode : std::uint16_t {
  Ok = 0,

  // --- generic input validation (1xx) -------------------------------------
  InvalidArgument = 100,
  EmptyField = 101,
  FieldTooLong = 102,
  InvalidCharacter = 103,
  ValueOutOfRange = 104,
  DuplicateElement = 105,
  TooManyElements = 106,
  MalformedEncoding = 107,
  ReservedFieldNotZero = 108,
  UnknownEnumValue = 109,

  // --- authority, generations, fencing (2xx) ------------------------------
  MissingAuthority = 200,
  StaleIncarnation = 201,
  StaleEpoch = 202,
  FutureEpoch = 203,
  StaleRevision = 204,
  FutureRevision = 205,
  StaleGeneration = 206,
  FutureGeneration = 207,
  CrossIncidentAuthority = 208,
  AuthorityFenced = 209,
  AuthorityRequired = 210,

  // --- incident lifecycle and state machine (3xx) -------------------------
  PreconditionFailed = 300,
  IllegalTransition = 301,
  NoActiveIncident = 302,
  IncidentNotFound = 303,
  IncidentClosed = 304,
  SeverityRegressionRefused = 305,
  RecoveryNotEligible = 306,
  GateNotSatisfied = 307,
  EvidenceNotCurrent = 308,
  IncidentAlreadyOpen = 309,

  // --- thermal evidence intake (4xx) -------------------------------------
  EvidenceRejected = 400,
  EvidenceStale = 401,
  EvidenceFuture = 402,
  EvidenceOutOfOrder = 403,
  EvidenceContradictory = 404,
  EvidenceUnavailable = 405,
  EvidenceUnknownTarget = 406,
  EvidenceDuplicate = 407,

  // --- protected obligations (5xx) ---------------------------------------
  ProtectedObligationViolation = 500,
  ObligationNotRelaxable = 501,
  ObligationUnknown = 502,
  ObligationViolated = 503,
  ObligationAlreadyRelaxed = 504,
  ObligationNotRelaxed = 505,

  // --- mitigation requests (6xx) -----------------------------------------
  RequestNotFound = 600,
  RequestStateConflict = 601,
  RequestExpired = 602,
  IdempotencyConflict = 603,
  MitigationUnverified = 604,
  RequestTargetMismatch = 605,
  MitigationFailed = 606,
  TransportFailure = 607,

  // --- resource bounds (7xx) ---------------------------------------------
  ResourceExhausted = 700,
  BoundsExceeded = 701,
  CheckpointUnavailable = 702,

  // --- persistence (8xx) --------------------------------------------------
  StoreNotFound = 800,
  StoreCorrupt = 801,
  StoreVersionUnsupported = 802,
  StoreTruncated = 803,
  StoreTrailingBytes = 804,
  StoreIntegrityFailure = 805,
  StoreLocked = 806,
  StorePathInvalid = 807,
  StoreIoError = 808,
  StoreReadbackMismatch = 809,
  NoAuthoritativeGeneration = 810,
  ReplayDivergence = 811,
  StoreAlreadyExists = 812,

  // --- runtime lifecycle and concurrency (9xx) ---------------------------
  RuntimeClosed = 900,
  ShutdownInProgress = 901,
  ReentrancyRefused = 902,
  NotSupported = 903,
  Internal = 904,
  ConcurrencyConflict = 905,
};

// Stable machine-readable identifier, e.g. "tem.stale_epoch".
[[nodiscard]] std::string_view status_code_name(StatusCode code) noexcept;

// True when the code is Ok.
[[nodiscard]] constexpr bool status_ok(StatusCode code) noexcept { return code == StatusCode::Ok; }

// Decodes a wire value; returns nullopt for values that are not assigned codes.
[[nodiscard]] std::optional<StatusCode> status_code_from_value(std::uint16_t value) noexcept;

// A machine-readable code plus a human-readable explanation. Explanations are
// diagnostics only: callers must branch on the code, never on the text.
class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  // Named success() rather than ok() so that the static factory and the
  // non-static predicate can coexist without an overload conflict.
  [[nodiscard]] static Status success() noexcept { return Status{}; }
  [[nodiscard]] static Status error(StatusCode code, std::string message) {
    return Status{code, std::move(message)};
  }

  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] std::string_view code_name() const noexcept { return status_code_name(code_); }

  // Optional structured context: the reference, id, or field the code applies
  // to. Bounded by the caller; used by diagnostics and audit output.
  [[nodiscard]] const std::string& context() const noexcept { return context_; }
  Status& with_context(std::string context) {
    context_ = std::move(context);
    return *this;
  }

  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_;
  }

 private:
  StatusCode code_{StatusCode::Ok};
  std::string message_{};
  std::string context_{};
};

// Programmer-error trap for impossible states. Aborts rather than throwing so
// that no exception ever crosses the public API boundary.
[[noreturn]] void trap(const char* expression, const char* file, int line);

#define TEM_TRAP(expr) ::summon::tem::trap((expr), __FILE__, __LINE__)

// Result<T>: either a value or a Status. Void results use the specialization.
//
// The error accessor is deliberately named status(). Because a member function
// of that name hides the class name Status inside this class scope, every type
// use below is fully qualified.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(::summon::tem::Status error)           // NOLINT(google-explicit-constructor)
      : value_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(value_); }
  [[nodiscard]] StatusCode code() const noexcept {
    return ok() ? StatusCode::Ok : std::get<::summon::tem::Status>(value_).code();
  }
  // Precondition: !ok(). Violating it is a programmer error and traps.
  [[nodiscard]] const ::summon::tem::Status& status() const noexcept {
    if (ok()) {
      TEM_TRAP("Result::status() called on a successful result");
    }
    return std::get<::summon::tem::Status>(value_);
  }
  [[nodiscard]] ::summon::tem::Status status_or_ok() const {
    return ok() ? ::summon::tem::Status::success()
                : std::get<::summon::tem::Status>(value_);
  }
  // Precondition: ok(). Violating it is a programmer error and traps.
  [[nodiscard]] T& value() & noexcept {
    if (!ok()) {
      TEM_TRAP("Result::value() called on an error result");
    }
    return std::get<T>(value_);
  }
  [[nodiscard]] const T& value() const& noexcept {
    if (!ok()) {
      TEM_TRAP("Result::value() called on an error result");
    }
    return std::get<T>(value_);
  }
  [[nodiscard]] T&& value() && noexcept {
    if (!ok()) {
      TEM_TRAP("Result::value() called on an error result");
    }
    return std::move(std::get<T>(value_));
  }
  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<T>(value_) : std::move(fallback);
  }

 private:
  std::variant<T, ::summon::tem::Status> value_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(::summon::tem::Status error)  // NOLINT(google-explicit-constructor)
      : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  [[nodiscard]] StatusCode code() const noexcept { return error_.code(); }
  [[nodiscard]] const ::summon::tem::Status& status() const noexcept { return error_; }
  [[nodiscard]] ::summon::tem::Status status_or_ok() const { return error_; }

 private:
  ::summon::tem::Status error_{};
};

using VoidResult = Result<void>;

}  // namespace summon::tem
