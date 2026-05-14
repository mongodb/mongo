# SERVER-44539 — Re-enable "missing RWC" logging (design proposal)

## Problem

After SERVER-43712 / SERVER-43126 closed the inter-node-RWC gaps, two LOGV2
calls in `service_entry_point_shard_role.cpp` were left commented to suppress
log spam from then-noisy callers. They are the only operator-visible signal
that an external command landed on a shard/config server without explicit RWC,
and they have been off for ~6 years. Re-enable them safely.

## Log-restore sites (exact file + line, w3-49 worktree)

`src/mongo/db/service_entry_point_shard_role.cpp`

- **L1370–L1376** — `RunCommandImpl::_validateAndSetWriteConcern`, inside the
  `ClusterRole::ShardServer || ClusterRole::ConfigServer` arm, gated on
  `!genericArgs.getWriteConcern()`. Restore `LOGV2(21959, "Missing
  writeConcern on command", "command"_attr = command->getName())`.
- **L1504–L1511** — `ExecCommandDatabase::_extractReadConcern`, inside the
  `ShardServer || ConfigServer` arm, gated on `!readConcernArgs.isSpecified()`.
  Restore `LOGV2(21954, "Missing readConcern for command", "command"_attr =
  _invocation->definition()->getName())`.

Both sites are already guarded against (a) direct clients, (b) internal
clients (which uassert on missing RWC at L1365 / L1498), and (c) multi-doc
transactions on non-transaction commands. That leaves only true external
clients — exactly the population we want surfaced.

## Proposal — three-step restore

1. **Re-enable behind `logv2`'s native severity, not a guard.** Uncomment the
   two `LOGV2` calls verbatim. No new server parameter; if spam returns the
   right answer is to fix the caller, not to gate the warning.
2. **Add `tassert`-free idempotency.** The lines fire per-command, so a
   misbehaving sweep could flood. Pre-emptively wrap each call with
   `logv2::SeveritySuppressor` keyed on `(command->getName(), clientRole)` at
   1/sec/key — matches the pattern used by SERVER-50601 for the
   `slowOperationLogger`. Keeps the line useful in steady state and rate-caps
   any regression.
3. **Pin both lines with `jstests/replsets/missing_rwc_logging.js`.** The test
   ships in this PR (see the sibling file) and starts a `ReplSetTest` with
   `--shardsvr` so the shard-role branch is reachable from a `replsets/` test.
   While the `LOGV2` lines stay commented the test asserts the ids are absent;
   when step 1 lands, the reviewer swaps the two `expectMissing` calls for
   `checkLog.containsJson` — one line per id, mechanical diff.

## Risks + audit hooks

- **Log spam.** Mitigated by step 2; further hedged by the fact that
  SERVER-45692 closed the internal-client side and SERVER-43712 / SERVER-43126
  closed the in-tree mongos paths. Remaining emitters are external drivers
  that explicitly choose to omit RWC.
- **jstests that exercise direct-to-shard commands without RWC.** Pre-scan
  `jstests/sharding/` for `_configsvrRunRestore`, `directConnect: true`, and
  raw `runCommand` against `rs.getPrimary()` of a shard; tag any that produce
  the new lines with `// SERVER-44539 expected: missing RWC log` and either
  add explicit `writeConcern: {}` or accept the diagnostic.
- **Operator dashboards.** The re-enabled `id: 21954 / 21959` lines should be
  added to the SRE log-anomaly allowlist before merge so the first 24h of
  prod traffic doesn't trip a synthetic alert.

## Out of scope

- Promoting the log to `ERROR` or making it a `uassert` (would be a behavior
  change for external clients — file a follow-up if desired).
- Re-classifying the shard-role-default-RWC story for mongos-to-config
  routing — that's SERVER-43126 territory and already shipped.

## Validation plan

1. Land the jstest in the disabled (`assert.throws`) configuration as part of
   this PR. CI green confirms the log lines are still absent today.
2. In the same PR (or a tight follow-up), uncomment both `LOGV2` calls and
   flip the two test lines per the in-file comment. CI green now confirms
   both ids fire under the documented preconditions.
3. Run the sharding suite resmoke selector with `--log=file --logComponentVerbosity='{command:1}'`
   on a 1-shard config and grep for `21954`/`21959` counts; expect ≤ O(test-count)
   firings after the per-name suppressor lands, not per-op.
