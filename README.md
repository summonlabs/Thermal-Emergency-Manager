# Thermal Emergency Manager

Thermal Emergency Manager (TEM) is repository 32 of the Data Center Control Plane
(DCCP) program. It owns facility thermal-emergency coordination: the authoritative
emergency state during a thermal excursion, the bounded cross-domain mitigations
that must be requested, the protected obligations that constrain those requests,
the conditions under which escalation must continue, and the current evidence
required before recovery may begin.

Version 1.0.0.

## The question this repository answers

During a thermal excursion:

* what emergency state is authoritative;
* which bounded cross-domain mitigations must be requested now;
* which protected obligations constrain those requests;
* when must escalation continue;
* what current evidence is required before safe recovery can begin.

## Owned boundary

TEM owns, end to end:

* incident and emergency identity, including incident generations;
* the severity ladder and the emergency lifecycle state machine;
* emergency authority scope, control epoch, and controller incarnation;
* emergency thermal evidence intake and its freshness classification;
* deterministic escalation planning and ordering;
* bounded derating requests;
* workload and facility drain requests;
* power-reduction requests;
* equipment-isolation requests;
* protected-obligation references and the relaxation rules that apply to them;
* acknowledgement and observation of requested mitigations;
* recovery gates and hysteresis;
* durable audit, replay, recovery, and stale-authority fencing.

## Explicit non-ownership

TEM never performs any of the following, and its public API cannot be used to do
so:

* scheduling or migrating workloads (ASI);
* facility placement;
* actuating power equipment;
* load shedding;
* actuating airflow or liquid-cooling devices;
* owning cooling failover;
* owning incident state beyond the thermal-emergency domain;
* bypassing safety interlocks;
* claiming an external mitigation effect from an acknowledgement.

Adjacent authorities are addressed only through typed, attributable requests, and
their state arrives only as typed, generation-stamped evidence or as opaque
references. Facility topology is never discovered or decided here: power domains
and isolatable equipment are supplied as opaque references by the authority that
owns them.

## Architecture

The runtime is a C++20 library (namespace `summon::tem`) with a small administration
CLI, an example, a benchmark, a test suite, and an installable CMake package.

| Header | Contents |
| --- | --- |
| `tem/status.hpp` | stable machine-readable `StatusCode` values, `Status`, `Result<T>` |
| `tem/ids.hpp` | strongly typed identities, generations, epochs, revisions, sequences |
| `tem/units.hpp` | exact integer quantities, checked arithmetic, timestamps and durations |
| `tem/refs.hpp` | canonically validated opaque reference tokens per kind |
| `tem/authority.hpp` | authority tokens and deterministic fencing precedence |
| `tem/policy.hpp` | thresholds, hysteresis, dwell windows, and resource bounds |
| `tem/evidence.hpp` | thermal samples, probe and zone assessment, justified severity |
| `tem/severity.hpp` | the severity ladder and lifecycle transition legality |
| `tem/obligations.hpp` | protected obligations, protection classes, relaxation policy |
| `tem/mitigation.hpp` | the four bounded mitigation classes, states, fingerprints |
| `tem/plan.hpp` | deterministic escalation planning |
| `tem/recovery.hpp` | recovery gates, hysteresis, eligibility |
| `tem/journal.hpp` | the authoritative input journal and transition records |
| `tem/state.hpp` | domain state, derivation, and the single `apply` mutator |
| `tem/store.hpp` | the dual-slot durable store, the OS lock, and snapshot codecs |
| `tem/runtime.hpp` | the public `EmergencyRuntime` API |
| `tem/scenario.hpp` | the synthetic plant and scenario engine |
| `tem/report.hpp` | text rendering for inspection and audit output |

### Severity ladder

`Nominal < Advisory < Warning < Critical < Emergency < Catastrophic`.

Severity is latched. While an incident is `Active`, `Stabilizing`, or `Recovering`, a
tick may only raise it. It may fall only through the recovery path, one level per
completed recovery step, and never below the level that current evidence
justifies.

The level justified by evidence is computed from current readings only:

* a fresh zone reading at or above a threshold justifies that level;
* a rise at or above `rapid_rate` with a reading at or above `rapid_rate_floor`
  justifies at least `Critical`;
* a rise at or above `extreme_rate` with a reading at or above `extreme_rate_floor`
  justifies at least `Emergency`;
* when `escalate_on_evidence_loss` is set and any monitored zone is not fresh, the
  result is at least `Warning`: lost evidence is never read as recovery.

### Lifecycle

`None -> Active -> Stabilizing -> Recovering -> Recovered -> Closed`, with the
documented regressions `Stabilizing/Recovering/Recovered -> Active`. `Closed` is
terminal; a later excursion opens a new incident with a new identity and a new
generation. Recovery is never started implicitly: an operator authority must
issue an explicit recovery request while the incident is `Stabilizing`.

### Mitigation classes and required levels

| Class | Required from | Target kind | Intensity ladder (basis points) |
| --- | --- | --- | --- |
| `DerateAccelerators` | Warning | thermal zone | 3000 / 5000 / 7000 / 9000 |
| `DrainWorkload` | Critical | thermal zone | 2500 / 5000 / 10000 |
| `ReducePower` | Emergency | power domain | 2000 / 5000 |
| `IsolateEquipment` | Catastrophic | equipment | 10000 |

Requests are planned in class order, then target byte order, so two runs over the
same evidence produce the same action sequence.

### Request state machine

`Planned -> Issued -> Acknowledged -> Observed -> Verified`, with the terminal
states `Failed`, `Superseded`, `Abandoned`, `Expired`, and `Refused`.

An acknowledgement is not an observation, and an observation is not a
verification. Only `Verified` satisfies a required mitigation class, and only
independent external evidence marked as such moves a request to `Verified`.
Verification evidence has a validity window: a confirmation that is no longer
current stops counting, and the owning authority reaffirms it.

A request that cannot be verified keeps its uncertainty: it fails on its
verification deadline, and escalation continues with a new attempt, up to a
bounded number of attempts per requirement.

### Protected obligations

| Protection class | Emergency authority may relax |
| --- | --- |
| `HardSafetyInterlock` | never |
| `Regulatory` | never |
| `AdvisoryOptimization` | only when the relaxation policy explicitly allows the class, and only with the granting authority recorded |

An obligation that is violated, unknown, unreported, or whose status report is no
longer current blocks recovery. The automatic escalation path is constrained by
obligations exactly as an explicit request is: there is no route around them.

## Authority, generations, and fencing

Every authority-bearing mutation carries an authority token: incident, incident
generation, control epoch, controller incarnation, and the state revision the
caller observed. Validation precedence is fixed, so the same invalid request
always produces the same primary machine-readable code:

1. missing authority (zero epoch or incarnation);
2. stale epoch; 3. future epoch;
4. stale incarnation;
5. incident-bound token with no live incident; 6. cross-incident token;
7. stale generation; 8. future generation;
9. stale revision; 10. future revision.

Opening a store that already holds durable state is itself fenced:

* a lower control epoch than the durable one is refused;
* the same epoch with a different incarnation is refused: a new controller must
  roll the epoch explicitly;
* a higher epoch performs a rollover: every non-terminal request from the
  previous authority is superseded, recovery progress is reset, and the new
  authority must re-establish recovery from current evidence.

Restored durable state is not current physical evidence. On every reopen, all
restored observations are marked as recovered and count for nothing until a live
sample from the same probe replaces them, and any dispatch that was in flight
when the previous incarnation stopped is resolved as indeterminate rather than
retried under the same attempt identity.

## Persistence and recovery

The store is a single directory containing two slot files and an OS lock file.

* Each slot holds a complete snapshot: a 48-byte header (magic, format version,
  reserved field, store generation, commit sequence, commit instant, payload
  length, payload CRC-32C, header CRC-32C), a payload, and a 16-byte trailer
  (magic plus total length).
* The payload carries the checkpoint state, the live state, and the retained
  journal entries, each length-prefixed.
* A commit encodes, bounds-checks, stages into a temporary file in the same
  directory, writes it, flushes it to the device, reads it back and compares it,
  and finally replaces the inactive slot atomically. The commit point is that
  atomic replacement: before it, the previous slot is authoritative.
* The authoritative slot is the valid slot with the highest commit sequence. If
  the newest slot is unusable, the store falls back to the previous complete
  generation and reports that it did so.
* Decoding is strict: magic, format version, reserved fields, header and payload
  checksums, exact length agreement, exact trailing-byte rejection, hard bounds
  on every declared length before allocation, and rejection of impossible enum
  values.
* Every load verifies the replay invariant: applying the retained journal to the
  checkpoint must reproduce the persisted live state byte for byte. A mismatch is
  reported as a replay divergence, not silently accepted.
* Single-writer authority is an OS-level exclusive handle on a canonicalised lock
  file inside the store directory, so two processes cannot hold the same store
  and a killed process cannot leave a stale lock.

The journal is bounded. When the bound is reached, the oldest entries are applied
to the checkpoint and retired, so replay always starts from a complete
checkpoint. The retirement count is store metadata and is deliberately outside
the replay equality check.

## Concurrency model

One writer. Durable mutation is serialised by the OS lock, in-process state by a
single mutex. Adapter code is never called with that mutex held: a tick computes
and persists its decisions, releases the lock, dispatches to the adjacent
authority, and then records the answers in a second committed step. The
persisted intent is therefore the write-ahead record for every external request.
A transport that calls back into the runtime is refused with
`tem.reentrancy_refused` instead of deadlocking. A failed durable commit fences
the runtime: no further authority-bearing operation is accepted until it is
reopened from the store.

## Build, test, and install

Requirements: CMake 3.25 or newer, and a C++20 compiler. Windows/MSVC is the
primary target; the core is portable.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

Debug and sanitizer configurations:

```sh
cmake -S . -B build-debug -G "Visual Studio 17 2022" -A x64
cmake --build build-debug --config Debug
ctest --test-dir build-debug -C Debug --output-on-failure

cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DTEM_ENABLE_ASAN=ON
cmake --build build-asan --config RelWithDebInfo
ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure
```

First-party code is built with `/W4 /permissive- /WX` on MSVC (and
`-Wall -Wextra -Wpedantic -Wshadow -Werror` elsewhere). No warning is globally
suppressed.

CMake options: `TEM_BUILD_TESTS`, `TEM_BUILD_TOOLS`, `TEM_BUILD_EXAMPLES`,
`TEM_BUILD_BENCHMARKS`, `TEM_WARNINGS_AS_ERRORS`, `TEM_ENABLE_ASAN`.

## Consuming the installed package

The installed package exports the namespaced target `Summon::tem`:

```cmake
find_package(ThermalEmergencyManager 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE Summon::tem)
```

`tests/consumer` is an independent out-of-tree project that configures against
an installed prefix, links the exported target, runs a real lifecycle (open a
durable runtime, register a facility, admit evidence, escalate, verify replay),
and removes its own store. It never references the build tree.

## Examples, CLI, and benchmark

`examples/thermal_excursion.cpp` drives a complete excursion through the library:
detection, escalation, bounded requests, external confirmation, stabilization,
explicit recovery, closure, and store/replay verification.

`temctl`:

```sh
temctl version
temctl demo [--store DIR] [--keep]
temctl scenario --name full-excursion
temctl scenario --all
temctl inspect --store DIR
temctl verify --store DIR
```

`tem_bench` measures completed operations only, with warm-up runs discarded:

```sh
tem_bench --iterations=1500
```

## Tests

Six test binaries, 65 test cases, run plainly with no timeout mechanism:

| Binary | Proof obligations |
| --- | --- |
| `tem_unit_tests` | units and checked arithmetic, identity typing, reference validation precedence, status codes, policy validation, obligation validation, fingerprint sensitivity, codec round trips, checksums |
| `tem_state_machine_tests` | ladder monotonicity, no illegal regression, evidence-loss floor, hysteresis and dwell, partial mitigation, acknowledgement versus verification, obligation enforcement, authority fencing, idempotent retry, deterministic dispatch order, byte-for-byte replay, an independent reference model over seeded random temperatures |
| `tem_persistence_tests` | snapshot round trip, single-byte and structural corruption, truncation, trailing bytes, version and reserved-field rejection, dual-slot fallback, staged-file rejection, lock exclusivity and canonicalisation, journal retention, restored evidence, volatile mode |
| `tem_process_tests` | real OS processes: lock contention, a hard kill during commits, a fresh process resolving the store, epoch rollover fencing, an end-to-end child scenario |
| `tem_scenario_tests` | the synthetic plant model, all fifteen named scenarios, the end-to-end excursion, a durable excursion that survives reopen, restart and rollover fencing |
| `tem_adversarial_tests` | malformed and reordered evidence, duplicate registration, oversized inputs, target/class mismatch, reentrant transport, stale dispatch answers, concurrent mutation from four threads, idempotent shutdown, re-issued attempts |

## Real, synthetic, and unsupported proof

* **REAL** — the library, the CLI, the example, the benchmark, the durable store,
  the file lock, crash and kill behaviour of real OS processes, package install,
  and downstream `find_package` consumption are exercised as software on this
  host. All test, benchmark, and validation results below were produced by real
  processes and real files.
* **SYNTHETIC** — the facility model, its probes, its thermal evolution, and the
  adjacent authority that answers mitigation requests are simulated in-process.
  Every scenario, including the end-to-end excursion, is synthetic. No chiller,
  CDU, CRAH/CRAC, pump, valve, breaker, BMS, DCIM, or PLC was contacted.
* **UNSUPPORTED** — there is no hardware validation of any kind: no real thermal
  sensor, no real power or cooling equipment, no real BMS/DCIM integration, and
  no real interconnect with an ASI, DFI, or third-party facility authority.
  Timing behaviour against a physical plant is therefore unproven here.

## Validation performed

All results below were produced on this host (Windows, MSVC 19.44, x64, CMake
4.3.2, sixteen logical cores) from the repository state described by this
version.

* Release build: zero warnings with `/W4 /permissive- /WX`.
* Debug build: zero warnings, full test suite passes.
* AddressSanitizer build (`/fsanitize=address`, MSVC, RelWithDebInfo): full test
  suite passes with no sanitizer report.
* Test suite: 6 binaries, 65 cases, 100% passing in Release, Debug, and the
  sanitizer configuration.
* Real multiprocess proofs: a second process is refused the store lock, a process
  killed during durable commits always leaves a complete generation that a fresh
  process resolves and replays, and a restart under a new epoch fences the
  previous plan.
* Persistence proofs: single-byte payload and header corruption, truncation,
  trailing bytes, unsupported version, non-zero reserved field, an unusable
  newest slot with fallback to the previous generation, and leftover staging
  files that never become authoritative.
* Package proofs: install into a prefix, then configure, build, and run the
  independent out-of-tree consumer through `find_package`.
* Adversarial proofs: reentrant transport callbacks, a dispatch answer that
  arrives after the request was superseded, four-thread concurrent mutation with
  replay verification afterwards, and repeated open/close cycles.

### Benchmark

`tem_bench --iterations=1500` on the host described above, release build,
warm-up discarded, measuring completed operations only:

| Workload | Unit | Completed ops/s | Mean latency |
| --- | --- | --- | --- |
| Durable evidence admission (one probe sample per operation) | sample admitted and committed | 59.3 | 16.9 ms |
| Emergency decision evaluation (admission plus escalation tick) | completed decision cycle | 279,997 | 3.6 us |
| Durable escalation cycle (12 escalation steps) | completed escalation cycle | 3.1 | 325 ms |

Durable figures include the full commit cost: encode, stage, flush to the device,
read back, and atomically replace the inactive slot. Admission latency grows with
the retained journal, because each commit rewrites the complete snapshot; the
concurrency model is single-writer and every mutation is durable by default.
The facility model behind these numbers is synthetic; the commits, flushes, and
read-backs are real.

## Limitations

* No hardware validation exists in this repository. All facility behaviour is
  synthetic.
* Commit cost grows with the retained journal and the size of the persisted
  state, because a commit publishes a complete snapshot. The retention bound
  bounds that growth; it does not eliminate the trade-off.
* The durable store assumes a local filesystem with atomic same-volume
  replacement. Network filesystems with weaker rename semantics are not covered.
* Durability is verified by device flush plus read-back comparison. No
  power-loss testing of the storage device itself was performed.
* Verification of a mitigation is only as good as the external evidence
  reference: TEM records the reference and its currency, and never interprets it.
* Byte-for-byte determinism is proven for the persisted encoding and for replay;
  it is not claimed for text rendering.
* The synthetic scenario engine models mitigation effects as a reduction of the
  modelled heating rate. That is a modeling choice for exercising semantics, not
  a thermal simulation.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
