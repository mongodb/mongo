# Shared Index Build Knobs IDL — Design Note

**Ticket:** SERVER-126118
**Author:** mehar.grewal (substrate-contrib)
**Status:** Implementation in progress

## Problem

`two_phase_index_build_knobs.idl` accumulated two distinct flavours of server
parameter:

1. **Two-phase-only** — semantics tied to the replicated commit-quorum protocol
   and resumable-build interceptor installation.
2. **Build-mode-agnostic** — admission control and disk-space safety knobs that
   already apply (or should apply) to primary-driven index builds (PDIB) as
   well.

Mixing the two has three downstream costs:
- The file name lies about coverage; reviewers reasonably assume a knob there
  is two-phase-specific and avoid touching it from PDIB code paths.
- A PDIB-only build that links `primary_driven_index_build_knobs_gen` would
  also have to link `two_phase_index_build_knobs_gen` purely to pick up shared
  admission knobs, leaking an unintended dependency.
- Future PDIB-specific overrides of admission policy have nowhere clean to
  land.

## Extraction (this commit)

A new file `shared_index_build_knobs.idl` carries parameters whose semantics
apply to both build modes. Initial population:

| Parameter                              | Moved from                          | Reason for sharing                                                                 |
|----------------------------------------|-------------------------------------|------------------------------------------------------------------------------------|
| `maxNumActiveUserIndexBuilds`          | `two_phase_index_build_knobs.idl`   | Global admission cap on concurrent user index builds; PDIB scheduling honours it.  |
| `indexBuildMinAvailableDiskSpaceMB`    | `two_phase_index_build_knobs.idl`   | Disk-space safety floor enforced by `IndexBuildsCoordinator`, independent of mode. |

Parameters intentionally **not** moved:
- `enableIndexBuildCommitQuorum` — two-phase commit-quorum protocol only.
- `resumableIndexBuildMajorityOpTimeTimeoutMillis` — gates two-phase interceptor
  install; PDIB has no resumable-build equivalent.
- `primaryDrivenIndexBuildPrefetching` — PDIB-specific prefetch toggle, stays
  in `primary_driven_index_build_knobs.idl`.
- `useReadOnceCursorsForIndexBuilds` — already in the neutral
  `index_build_knobs.idl`; out of scope.

## Wire-level invariants (must hold post-refactor)

1. The C++ symbol names (`maxNumActiveUserIndexBuilds`,
   `gIndexBuildMinAvailableDiskSpaceMB`) and their types are identical to the
   pre-refactor declarations. No call-site change is required.
2. The generated server-parameter registration (`set_at`, `default`,
   `validator`, `redact`) is byte-equivalent to the prior declarations.
3. `mongo_cc_library(:index_build_knobs_idl, ...)` continues to surface both
   parameters to every consumer that depended on it. Adding
   `shared_index_build_knobs_gen` to the same `srcs` list preserves the
   externally observable target shape.

## Parity test

`jstests/noPassthrough/index_builds/shared_idl_knobs_present.js` asserts at
runtime that both shared parameters are reachable via `getParameter` /
`setParameter` with their documented defaults and types. It is intentionally
mode-agnostic: it does not start an index build, only the server, and exists
to catch a wrong-side BUILD edit that drops the new IDL target from the
linker.

## Migration path (deferred)

A follow-up may rename `index_build_knobs.idl` to something less generic and
fold its single remaining parameter into `shared_index_build_knobs.idl`. That
is an isolated rename, blocked on this ticket landing first.
