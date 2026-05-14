# SERVER-126509 — Migration plan: consolidate all-index-build knobs into `index_build_knobs.idl`

Scope: enumerate which server parameters move into `index_build_knobs.idl` and pin parity with jstests. **Plan only — no IDL fields renamed in this change.**

## Inclusion rule

A knob moves iff it governs **every** index build path (foreground, hybrid, two-phase, primary-driven, resumable). Knobs scoped to a single path stay in their current IDL.

## Knobs to move into `index_build_knobs.idl`

| Knob | Current home | Rationale |
|---|---|---|
| `maxIndexBuildMemoryUsageMegabytes` | `multi_index_block.idl` | Bounds every collection-level build; not hybrid-specific. |
| `internalIndexBuildBulkLoadYieldIterations` | `multi_index_block.idl` | Yield cadence applies to all bulk loaders. |
| `indexBuildSpillingMinAvailableDiskSpaceBytes` | `multi_index_block.idl` | Disk-space gate fires regardless of build mode. |
| `maxIndexBuildDrainBatchSize` | `index_build_interceptor.idl` | Drain runs for every hybrid/two-phase/PDIB build. |
| `maxIndexBuildDrainMemoryUsageMegabytes` | `index_build_interceptor.idl` | Same drain phase, universal. |
| `indexBuildMinAvailableDiskSpaceMB` | `two_phase_index_build_knobs.idl` | Misnamed; gate runs for foreground builds too. |
| `maxNumActiveUserIndexBuilds` | `two_phase_index_build_knobs.idl` | Coordinator-level concurrency cap, not two-phase-only. |

## Knobs that stay put

- `two_phase_index_build_knobs.idl`: `enableIndexBuildCommitQuorum`, `resumableIndexBuildMajorityOpTimeTimeoutMillis` (two-phase replication semantics).
- `primary_driven_index_build_knobs.idl`: all `primaryDrivenIndexBuild*` knobs + `primaryDrivenIndexBuildPrefetching` (PDIB-only).

## BUILD.bazel impact

`index_build_knobs_idl` mongo_cc_library already aggregates the three knob generators. Removing the source knobs from `multi_index_block_gen` / `index_build_interceptor_gen` requires every dependent of those targets to add `:index_build_knobs_idl`. Audit `bazel query 'rdeps(//src/mongo/..., //src/mongo/db/index_builds:multi_index_block_gen)'` before flipping.

## Parity pinning (jstests, no behavior change expected)

Run unchanged against pre/post migration; values flow through the same `cpp_varname`s:

- `jstests/noPassthrough/index_builds/index_build_maximum_memory_usage.js` — `maxIndexBuildMemoryUsageMegabytes`
- `index_build_wildcard_memory_usage.js`, `index_build_memory_tracking.js` — same.
- `index_build_continuous_drain_secondary.js`, `resumable_index_build_drain_writes_phase{,_primary,_secondary}.js` — drain knobs.
- `create_indexes_fails_if_insufficient_disk_space.js`, `index_build_killed_disk_space{,_secondary}.js` — disk-space gates.
- `index_build_yield_bulk_load.js`, `serverstatus_indexbulkbuilder.js` — bulk-load yield + counters.
- `replsets/rollback_resumable_index_build_*_phase_large.js` — resume/rollback paths.

## Risk + sequencing

1. Land IDL move with `cpp_varname` unchanged (no `.js` edits).
2. CI signal = above jstest set + `index_builds_test` unit suite.
3. Follow-up ticket optional: rename `indexBuildMinAvailableDiskSpaceMB` for clarity.

Depends on SERVER-126439 (closed). Epic: SPM-4469.
