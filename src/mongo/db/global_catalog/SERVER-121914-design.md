# SERVER-121914 — FCV Downgrade Must Drain Pending Orphan Cleanup

## Background

Range deletion is the asynchronous half of a chunk migration. After a `moveChunk` commits, the
donor records a document in `config.rangeDeletions` describing the range whose ownership it just
gave up, marks it `pending: true`, and lets the in-process range deleter remove the orphan
documents in batches. The deletion document is flipped out of `pending: true` only after the
migration decision document has been persisted; only then is the range deleter actually allowed to
start the scan-and-delete loop.

`setFeatureCompatibilityVersion` aborts any in-flight chunk migrations during the downgrade
transitional state (`migration_util.cpp::abortMigrationsRunningAtFCVDowngrade`). The abort is
implemented by killing the migration coordinator's operation context; the donor then unwinds and
leaves the partially-completed state on disk.

The bug, which is a direct variant of SERVER-92484: if the abort lands in the window between the
migration's commit and the deletion-document state transition, the donor is left holding a
`pending: true` range-deletion doc whose orphans are not reclaimed. Range-deleter cleanup is gated
on `pending: false`, and the migration recovery path that would flip the bit only runs on the next
metadata refresh for that namespace or on the next step-up — neither of which the downgrade itself
guarantees. The recipient side may also be holding a sibling `pending: true` doc, which is what
defeats the SERVER-103749 workaround in `check_orphans_are_deleted_helpers.js`: the helper forces a
refresh on the donor only.

In production the impact is bounded (orphans waste disk until the next refresh/step-up and may
distort `find().itcount()`-style sanity checks), but the invariant "downgrade leaves no pending
orphan-cleanup work behind" is what users and the test infrastructure already assume.

## Proposed fix

Two viable shapes, both inside the FCV-downgrade transitional state:

1. **Synchronous drain before the FCV bit flips.** Before
   `setFCV` advances from `downgrading` to the target FCV, the config server iterates each shard
   and runs the equivalent of
   `_flushRoutingTableCacheUpdates` for every namespace appearing in `config.migrationCoordinators`
   on either the donor or recipient side, then waits for `config.rangeDeletions` to be empty (with
   `pending: true` documents flipped via the recovery path). The wait is bounded; the timeout
   surfaces as a `setFCV` failure with a structured error so an operator can retry rather than
   silently leak orphans.

2. **Scheduled post-condition.** The FCV downgrade returns success once all migrations have been
   aborted, and a new persistent background task — owned by the shard primary — completes the
   drain. The task is idempotent and survives step-down. It emits a structured warning log entry
   (id 10083100) when it observes a leaked `pending: true` document so the test harness and
   external monitoring can detect the gap deterministically.

Shape (1) is simpler and matches the user-facing contract that "downgrade is done means downgrade
is done"; shape (2) is cheaper to implement and survives a multi-shard cluster where one shard is
unhealthy at downgrade time. The recommendation is to ship shape (1) for new clusters and keep
shape (2) as the recovery hook for clusters that booted into the bad state on an older patch
release.

## Test coverage

`jstests/sharding/fcv_downgrade_orphan_cleanup.js` exercises the post-commit / pre-decision window
using the `hangBeforeWritingDecisionDocument` failpoint, runs `setFeatureCompatibilityVersion:
lastLTSFCV` against a running migration, and asserts within 60s that either the donor's orphan
count for the moved range drains to zero, or the structured warning (log id `10083100`) is emitted
identifying the leaked namespace. The two acceptance branches map directly onto the two fix shapes
above.

## Backport

`v8.0` and `v8.1` are affected (the abort-during-downgrade path landed in 8.0). Both branches
should receive the fix; the jstest backports unchanged since it only uses public failpoints and
shell helpers that exist on both branches.
