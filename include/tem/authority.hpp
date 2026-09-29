// Thermal Emergency Manager -- authority, incarnation, epoch, and fencing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <string>

#include "tem/ids.hpp"
#include "tem/status.hpp"

namespace summon::tem {

// Every authority-bearing mutation is bound to the exact state it was planned
// against. A token that does not match the live state is refused
// deterministically and never partially applied.
//
//   incident / generation : which emergency the command speaks for
//   epoch                 : which control-plane authority generation is acting
//   incarnation           : which controller process holds that authority
//   expected_revision     : the state revision the caller observed
struct AuthorityToken {
  IncidentId incident{};
  IncidentGeneration generation{};
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision expected_revision{};

  // A token for controller-scoped operations that are not bound to an
  // incident (registration before any excursion).
  [[nodiscard]] static AuthorityToken for_controller(ControlEpoch epoch,
                                                     ControllerIncarnation incarnation,
                                                     StateRevision revision) noexcept {
    return AuthorityToken{IncidentId{}, IncidentGeneration{}, epoch, incarnation, revision};
  }

  [[nodiscard]] std::string describe() const;
};

// The live authority and state a token is compared against.
struct AuthorityView {
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision revision{};
  IncidentId incident{};
  IncidentGeneration generation{};
  bool incident_present{false};
};

// Validation precedence (first failure wins, stable across releases):
//   1. zero epoch or zero incarnation          -> MissingAuthority
//   2. token.epoch < current.epoch             -> StaleEpoch
//   3. token.epoch > current.epoch             -> FutureEpoch
//   4. token.incarnation != current.incarnation-> StaleIncarnation
//   5. incident-bound token, no live incident  -> NoActiveIncident
//   6. token.incident != live incident         -> CrossIncidentAuthority
//   7. token.generation < live generation      -> StaleGeneration
//   8. token.generation > live generation      -> FutureGeneration
//   9. token.revision < live revision          -> StaleRevision
//  10. token.revision > live revision          -> FutureRevision
[[nodiscard]] Status check_authority(const AuthorityToken& token, const AuthorityView& live);

// True when the token carries an incident binding.
[[nodiscard]] inline bool token_is_incident_bound(const AuthorityToken& token) noexcept {
  return token.incident.is_set();
}

}  // namespace summon::tem
