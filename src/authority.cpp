// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "tem/authority.hpp"

#include <string>

namespace summon::tem {
namespace {

std::string append_u64(std::string& out, const char* label, std::uint64_t value) {
  out.append(label);
  out.append(std::to_string(value));
  return out;
}

}  // namespace

std::string AuthorityToken::describe() const {
  std::string out;
  append_u64(out, "incident=", incident.value());
  append_u64(out, " generation=", generation.value());
  append_u64(out, " epoch=", epoch.value());
  append_u64(out, " incarnation=", incarnation.value());
  append_u64(out, " revision=", expected_revision.value());
  return out;
}

Status check_authority(const AuthorityToken& token, const AuthorityView& live) {
  if (token.epoch.is_absent() || token.incarnation.is_absent()) {
    return Status::error(StatusCode::MissingAuthority,
                         "authority token must carry a control epoch and a controller incarnation");
  }
  if (token.epoch < live.epoch) {
    return Status::error(StatusCode::StaleEpoch,
                         "authority token carries a superseded control epoch")
        .with_context(token.describe());
  }
  if (live.epoch < token.epoch) {
    return Status::error(StatusCode::FutureEpoch,
                         "authority token carries a control epoch ahead of the live epoch")
        .with_context(token.describe());
  }
  if (!(token.incarnation == live.incarnation)) {
    return Status::error(StatusCode::StaleIncarnation,
                         "authority token was issued by a different controller incarnation")
        .with_context(token.describe());
  }
  if (token.incident.is_set()) {
    if (!live.incident_present) {
      return Status::error(StatusCode::NoActiveIncident,
                           "authority token is incident bound but no incident is open")
          .with_context(token.describe());
    }
    if (!(token.incident == live.incident)) {
      return Status::error(StatusCode::CrossIncidentAuthority,
                           "authority token belongs to a different incident")
          .with_context(token.describe());
    }
    if (token.generation < live.generation) {
      return Status::error(StatusCode::StaleGeneration,
                           "authority token carries a superseded incident generation")
          .with_context(token.describe());
    }
    if (live.generation < token.generation) {
      return Status::error(StatusCode::FutureGeneration,
                           "authority token carries an incident generation ahead of the live one")
          .with_context(token.describe());
    }
  }
  if (token.expected_revision < live.revision) {
    return Status::error(StatusCode::StaleRevision,
                         "authority token was planned against a superseded state revision")
        .with_context(token.describe());
  }
  if (live.revision < token.expected_revision) {
    return Status::error(StatusCode::FutureRevision,
                         "authority token was planned against a future state revision")
        .with_context(token.describe());
  }
  return Status::success();
}

}  // namespace summon::tem
