# SERVER-126285: Multikey path must not be set from aborted-transaction operations

## Symptom

An index build that runs concurrently with a transaction may end up marking a key
path multikey in the durable catalog even though the transaction that produced the
array-valued write was aborted. Once a path is recorded as multikey it never
reverts. From the planner's point of view the index is multikey for the rest of
the index's lifetime, even though no committed document in the collection actually
makes the path multikey. The result is a permanent regression in plan quality
(unnecessary index-bounds expansion and de-duplication work), and on replication
the primary's catalog disagrees with the secondary's — the secondary applies the
oplog, which never carries the aborted insert, so its interceptor never sees the
multikey-flipping write.

This is a poisoning bug, not a correctness bug for queries that simply hit the
index, but it falsifies the cross-node "same catalog state" invariant that the
disagg consistency checks rely on.

## Root cause

`IndexBuildInterceptor::sideWrite` (`src/mongo/db/index_builds/index_build_interceptor.cpp`)
performs two distinct effects when an insert arrives during an index build:

1. It buffers the keys into the side-writes table via `SideWritesTracker::bufferSideWrite`.
   The buffered row is a normal storage-engine write — it participates in the
   caller's `RecoveryUnit` and is correctly rolled back on transaction abort.
2. It unconditionally merges the operation's `multikeyPaths` into the
   interceptor's in-memory `_multikeyPaths` member (lines 228–233):

       if (_multikeyPaths) {
           MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.value(), multikeyPaths);
       } else {
           _multikeyPaths = multikeyPaths;
       }

Effect (2) bypasses the `RecoveryUnit`. There is no `onRollback` callback
registered, so a `WriteUnitOfWork::release()` (transaction abort) leaves the
in-memory tracker holding state from operations that never committed. When the
index build later finishes its second-drain phase and stamps the multikey state
into the durable catalog at commit-build timestamp, the dirty tracker wins.

On a secondary, the interceptor is driven by the oplog. The oplog only contains
*committed* operations, so the aborted insert is never seen and the secondary's
tracker stays clean. After the index commits, primary and secondary disagree.

## Fix

The fix is to make the multikey tracker update participate in the same atomic
unit as the storage write. Two equivalent shapes considered:

**Option A — defer to commit (preferred).**
Register an `onCommit` hook on the caller's `RecoveryUnit` from within
`sideWrite()`. The hook captures the proposed `multikeyPaths` by value and
merges it into `_multikeyPaths` only when the unit commits. On abort the
captured value is dropped with the rest of the unit. This is the same pattern
the catalog already uses for in-memory descriptor mutations
(`onCommit` / `onRollback` symmetry in `CollectionImpl`).

**Option B — filter at drain time by commitTimestamp.**
Keep the in-memory write but stamp it with the caller's prepare/commit
timestamp. At drain time (`drainWritesIntoIndex` / second-drain), reject merged
multikey entries whose timestamp lacks a matching committed side-table row.
This is more invasive and adds a per-drain scan.

We are taking Option A. It is local to `sideWrite()`, requires no schema
change to the side-writes table, and matches the existing catalog convention
that "in-memory mirrors of durable state are mutated under `onCommit`".

## Concurrency notes

`_multikeyPaths` is protected by `_multikeyPathsMutex`. The `onCommit` callback
runs on the commit thread of the originating `OperationContext`; the lock is
already taken in the existing critical section, so the merge can simply move
inside the lambda. Care is needed for `prepareTransaction` flows: the merge
must run on the actual commit hook, not on prepare, otherwise a prepared-then-
aborted transaction reintroduces the original bug. The existing
`OperationContext::RecoveryUnit::onCommit` registration runs only on the
terminal commit step, so this is satisfied without additional plumbing.

## Test

`jstests/noPassthrough/index_builds/multikey_not_set_from_aborted_txn.js`
drives a single-node replica set with majority writes, pauses an index build
on `{a: 1, b: 1}`, inserts `{a: [100, 200, 300], b: 999}` inside a
transaction, aborts the transaction, resumes the index build, and asserts
that the committed index does not report `isMultiKey` on field `a` via either
the explain plan or `collStats().indexDetails`. The test fails on unpatched
code and passes on patched code.
