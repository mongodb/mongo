# Replica Set Write Blocking

## Overview

Replica set write blocking prevents writes from being performed on a replica set when the set is at
risk of running out of disk space. It is a _local_, per-replica-set mechanism enabled via the
`blockReplicaSetWrites` command. The only reason for which replica set writes are currently blocked
is `kInsufficientDiskSpace` (see `replica_set_writes_block_reason.idl`); the reason is carried
through all of the state so that it can be surfaced in errors and metrics.

## Relationship to user write blocking

Replica set write blocking is distinct from [user write blocking](README_user_write_blocking.md):
user write blocking is an administrative, cluster-wide (or C2C-wide) block enabled via
`setUserWriteBlockMode`, whereas replica set write blocking is local and per-replica-set. The two
features live side by side, share the on-disk recoverable critical section machinery, and are
enforced by parallel `OpObserver`s, but they maintain completely independent state.

When both features are enabled, `setUserWriteBlockMode` takes precedence over replica set write
block.

Unlike user write blocking on a sharded cluster, replica set write blocking has no cluster-wide
coordinator and no two-phase, DDL-draining enable protocol. User write blocking is enabled on
`mongos` and coordinated by the config server, which first blocks new `ShardingDDLCoordinator`s and
drains the running ones before promoting the critical section to actually block writes. The
`blockReplicaSetWrites` command, by contrast, is issued directly against each shard (or the bare
replica set) and takes effect locally: there is no draining of sharding DDL coordinators, and no
config-server orchestration.

## The `blockReplicaSetWrites` command

Replica set write blocking is enabled and disabled by the `blockReplicaSetWrites` command
(`block_replica_set_writes_command.cpp`, defined in `block_replica_set_writes.idl`). The command is
shard-only (`.forShard()`), is never allowed on secondaries, is rejected on standalones, and is
gated behind the `gFeatureFlagBlockReplicaSetWrites` feature flag. Invoking it requires the
`blockReplicaSetWrites` action on the `cluster` resource. It takes three fields: `enabled` (whether
to block or unblock), `allowDeletions` (required when enabling, disallowed when disabling), and
`reason`.

A static mutex ensures that only one attempt to change the blocking state makes progress at a time.
Once the blocking state has been changed, the command waits for the write of the critical section
document to be majority committed.

## Interaction with setFeatureCompatibilityVersion

This command is not compatible with `setFeatureCompatibilityVersion`: the two are mutually
exclusive, enforced from both sides. `blockReplicaSetWrites` holds a `FixedFCVRegion` for its
duration — serializing it against `setFeatureCompatibilityVersion` — and rejects the request if an
FCV upgrade/downgrade is already in progress. Conversely, `setFeatureCompatibilityVersion` refuses
to upgrade or downgrade while replica set write blocking is enabled, failing with
`ErrorCodes::ReplicaSetWritesBlocked` (see `legacy_fcv_step.cpp`); the block must be disabled before
an FCV transition can proceed.

## Blocking deletions

The `allowDeletions` flag exists because deletions are not purely space-freeing: they can also
temporarily consume additional disk space (for example, through the writes they generate). The flag
therefore lets an operator choose. When deletions are allowed, an operator can block inserts and
updates while still permitting users to delete data and recover. When deletions are blocked, they
are treated like any other write and rejected. Blocking deletions also blocks compaction, which can
itself require disk space: both the on-demand `compact` command (`checkIfCompactAllowedToStart`) and
background auto-compaction (paused via `pauseOrResumeAutoCompactForWriteBlock`) are disallowed while
deletions are blocked.

## On-disk state and recovery

The blocking state is persisted on disk as a `ReplicaSetWriteBlockingCriticalSectionDocument`
(`replica_set_writes_critical_section_document.idl`) in the
`config.replica_set_writes_critical_section` collection, recording `enabled`, `allowDeletions`, and
`replicaSetWritesBlockReason`. This document is managed by the
`UserWritesRecoverableCriticalSectionService` (shared with user write blocking) through
`acquireRecoverableCriticalSectionBlockingReplicaSetWrites` and
`releaseRecoverableCriticalSectionBlockingReplicaSetWrites`. Because this is a _recoverable_
critical section, the in-memory state is rebuilt from disk whenever consistent data becomes
available (`recoverRecoverableCriticalSections`, invoked from `onConsistentDataAvailable`), so the
block survives restarts.

## In-memory state

The in-memory state lives in `ReplicaSetWriteBlockState` (`replica_set_write_block_state.h`/`.cpp`),
a `ServiceContext` decoration. It holds the atomic write-block flag and its reason, a separate
deletions-blocked flag, a transient index-build-block flag, and counters used for `serverStatus`
(`replicaSetWritesBlockCounters`, one per reason) and for rejection metrics
(`replicaSetWritesBlockRejected`, split into inserts/updates/deletes). It exposes the predicates
that decide whether an operation may proceed: `checkReplicaSetWritesAllowed`,
`checkReplicaSetDeletionsAllowed`, and the operation-specific gates `checkIfCompactAllowedToStart`,
`checkIfConvertToCappedAllowedToStart`, `checkIfIncomingMigrationAllowedToStart`,
`checkIfIncomingReshardingAllowedToStart`, and `checkIfIndexBuildAllowedToStart`. When a check fails
it throws (or returns) with `ErrorCodes::ReplicaSetWritesBlocked`.

## Enforcement

Enforcement and state synchronization are both performed by the `ReplicaSetWriteBlockOpObserver`
(`replica_set_write_block_op_observer.h`/`.cpp`). On `onInserts`, `onUpdate`, `onDelete`, and the
index-build-start hooks, it consults `ReplicaSetWriteBlockState`, but only when the node is a
replica set member that `canAcceptWritesFor` the target namespace — that is, on the primary write
path — so that secondaries applying the oplog are not blocked. Writes are always allowed through if
the namespace is on an internal database or is `system.profile`, or if the operation carries an
active bypass (see [Bypass](#bypass)). Deletions are gated separately by the `allowDeletions` flag;
this intentionally also blocks range deletions (orphan cleanup, which sets `fromMigrate = true`) and
TTL deletions. The same `OpObserver` watches writes to `config.replica_set_writes_critical_section`:
when the critical section document is inserted, updated, or deleted, it registers an `onCommit`
handler that flips the in-memory `ReplicaSetWriteBlockState` accordingly. Because this fires as the
document is replicated and applied, every node in the set converges to the same blocking state.

## Operations affected

While the block is active the following operations are affected (all subject to the bypass,
internal-database, and `system.profile` exemptions described above):

- **Blocked when writes are blocked:**
  - Plain user writes — inserts and updates.
  - `createIndex` / new index builds, via `checkIfIndexBuildAllowedToStart`
    (`index_builds_coordinator_mongod.cpp`).
  - `renameCollection` across databases, which copies documents into the target, via a
    `checkReplicaSetWritesAllowed` pre-check on the target namespace (`rename_collection.cpp`,
    `rename_collection_coordinator.cpp`).
  - `convertToCapped`, via `checkIfConvertToCappedAllowedToStart` (`capped_utils.cpp`).
  - `cloneCollectionAsCapped`, via a `checkReplicaSetWritesAllowed` pre-check on the target
    namespace (`collection_to_capped.cpp`).
  - `moveChunk` — the recipient side of a chunk migration, via
    `checkIfIncomingMigrationAllowedToStart` (`migration_destination_manager.cpp`).
  - `reshardCollection` — the recipient side, via `checkIfIncomingReshardingAllowedToStart`
    (`resharding_donor_recipient_common.cpp`).
  - `movePrimary` — the recipient's catalog-data cloning goes through the normal insert path, so the
    first data-cloning insert is rejected.
- **Blocked only when deletions are blocked** (i.e. `allowDeletions` was false): deletes — including
  range deletions (orphan cleanup) and TTL deletions — the on-demand `compact` command
  (`checkIfCompactAllowedToStart`), and background auto-compaction.
- **Held (paused and resumed):**
  - `reshardCollection` already in progress on the recipient — the resharding recipient treats
    `ErrorCodes::ReplicaSetWritesBlocked` as a transient error and hold, resuming once the block is
    lifted, instead of aborting the operation (`resharding_collection_cloner.cpp`,
    `resharding_txn_cloner.cpp`, `resharding_oplog_batch_applier.cpp`).
- **Aborted:**
  - Index builds — when the block is first enabled, all in-progress index builds are aborted (and
    those that cannot be aborted are drained) via
    `IndexBuildsCoordinator::abortIndexBuildsForWriteBlocking`, and new index builds are held off
    during setup until the `OpObserver` takes over enforcement.
  - `movePrimary` — the recipient's catalog-data cloning goes through the normal insert path (it
    does not enable the bypass), so those inserts are rejected. Because the clone step
    (`_shardsvrCloneCatalogData`) is non-idempotent, encountering the block aborts the whole
    movePrimary operation rather than holding or retrying it (`move_primary_coordinator.cpp`).

## Bypass

Certain operations must be able to proceed even while writes are blocked — for example, migration
traffic. This is handled by `ReplicaSetWriteBlockBypass`
(`replica_set_write_block_bypass.h`/`.cpp`), an `OperationContext` decoration that mirrors the
`WriteBlockBypass` mechanism used by user write blocking. Every `check...Allowed` predicate consults
it. On internal requests the originator propagates the bypass through the
`mayBypassReplicaSetWritesBlocking` request-metadata field (`writeAsMetadata` / `setFromMetadata`);
this field is only honored from clients that hold the `internal` action on the `cluster` resource.
On external requests the bypass is derived from the `AuthorizationSession`
(`AuthorizationSession::mayBypassReplicaSetWritesBlocking`), which is true for a user that holds the
`bypassReplicaSetWritesBlocking` action on the `cluster` resource — granted through the
`clusterManager` built-in role (and roles that include it, such as `clusterAdmin` and `root`). This
is the replica-set analogue of user write blocking's `bypassWriteBlockingMode` action. This
propagation model ensures, for example, that internal writes issued on behalf of an operation that
began before the block are not spuriously rejected mid-flight.
