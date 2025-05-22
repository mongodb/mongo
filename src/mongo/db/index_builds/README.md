# Index Builds

Indexes are built by performing a full scan of collection data. To be considered consistent, an
index must correctly map keys to all documents.

At a high level, omitting details that will be elaborated upon in further sections, index builds
have the following procedure:

- While holding a collection X lock, write a new index entry to the array of indexes included as
  part of a durable catalog entry. This entry has a `ready: false` component. See [Durable
  Catalog](../catalog/README.md#durable-catalog).
- Downgrade to a collection IX lock.
- Scan all documents on the collection to be indexed
  - Generate [KeyString](../storage/key_string/README.md) keys for the indexed fields for each
    document
  - Periodically yield locks and storage engine snapshots
  - Insert the generated keys into the [external sorter](../sorter/README.md)
- Read the sorted keys from the external sorter and [bulk
  load](http://source.wiredtiger.com/3.2.1/tune_bulk_load.html) into the storage engine index.
  Bulk-loading requires keys to be inserted in sorted order, but builds a B-tree structure that is
  more efficiently filled than with random insertion.
- While holding a collection X lock, make a final `ready: true` write to the durable catalog.

## Hybrid Index Builds

Hybrid index builds refer to the default procedure introduced in 4.2 that produces efficient index
data structures without blocking reads or writes for extended periods of time. This is achieved by
performing a full collection scan and bulk-loading keys (described above) while concurrently
intercepting new writes into a temporary storage engine table.

### Temporary Side Table For New Writes

During an index build, new writes (i.e. inserts, updates, and deletes) are applied to the collection
as usual. However, instead of writing directly into the index table as a normal write would, index
keys for documents are generated and intercepted by inserting into a temporary _side-writes_ table.
Writes are intercepted for the duration of the index build, from before the collection scan begins
until the build is completed.

Both inserted and removed keys are recorded in the _side-writes_ table. For example, during an index
build on `{a: 1}`, an update on a document from `{_id: 0, a: 1}` to `{_id: 0, a: 2}` is recorded as
a deletion of the key `1` and an insertion of the key `2`.

Once the collection scan and bulk-load phases of the index build are complete, these intercepted
keys are applied directly to the index in three phases:

- Drain the side table while holding a collection IX lock to allow concurrent reads and writes.
  - Since writes are still accepted, new keys may appear at the end of the _side-writes_ table.
    They will be applied in subsequent steps.
    (Signal commit readiness to the primary)
- Continue draining the side table while holding a collection IX lock to allow concurrent reads and
  writes, while waiting for other replicas to become commit-ready.
- Drain the side table while holding a collection X lock to block all reads and writes.

See
[IndexBuildInterceptor::sideWrite](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/index_build_interceptor.cpp#L403)
and
[IndexBuildInterceptor::drainWritesIntoIndex](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/index_build_interceptor.cpp#L135).

### Temporary Table For Duplicate Key Violations

Unique indexes created with `{unique: true}` enforce a constraint that there are no duplicate keys
in an index. The hybrid index procedure makes it challenging to detect duplicates because keys are
split between the bulk-loaded index and the side-writes table. Additionally, during the lifetime of
an index build, concurrent writes may introduce and resolve duplicate key conflicts on the index.

For those reasons, during an index build we temporarily allow duplicate key violations, and record
any detected violations in a temporary table, the _duplicate key table_. At the conclusion of the
index build, under a collection X lock, [duplicate keys are
re-checked](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator.cpp#L2312).
If there are still constraint violations, an error is thrown.

See
[DuplicateKeyTracker](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/duplicate_key_tracker.h#L48).

### Temporary Table For Key Generation Errors

In addition to uniqueness constraints, indexes may have per-key constraints. For example, a compound
index may not be built on documents with parallel arrays. An index build on `{a: 1, b: 1}` will fail
to generate a key for `{a: [1, 2, 3], b: [4, 5, 6]}`.

On a primary under normal circumstances, we could fail an index build immediately after encountering
a key generation error. Since secondaries apply oplog entries [out of
order](../repl/README.md#oplog-entry-application), however, spurious key generation errors may be
encountered on otherwise consistent data. To solve this problem, we can relax key constraints and
suppress key generation errors on secondaries.

With the introduction of simultaneous index builds, an index build may be started on a secondary
node, but complete while it is a primary after a state transition. If we ignored constraints while
in the secondary state, we would not be able to commit the index build and guarantee its consistency
since we may have suppressed valid key generation errors.

To solve this problem, on both primaries and secondaries, the records associated with key generation
errors are skipped and recorded in a temporary table, the _skipped record table_. Like duplicate key
constraints, but only on primaries at the conclusion of the index build, the keys for the [skipped
records are
re-generated](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator.cpp#L2294)
and re-inserted under a collection X lock. If there are still constraint violations, an error is
thrown. Secondaries rely on the primary's decision to commit as assurance that skipped records do
not need to be checked.

See
[SkippedRecordTracker](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/index/skipped_record_tracker.h#L45).

## Replica Set Index Builds

Also referred to as "simultaneous index builds" and "two-phase index builds".

As of 4.4, index builds in a replica set use a two-phase commit protocol. When a primary starts an
index build, it spawns a background thread and replicates a `startIndexBuild` oplog entry. Secondary
nodes will start the index build in the background as soon as they apply that oplog entry. When a
primary is done with its indexing, it will decide to replicate either an `abortIndexBuild` or
`commitIndexBuild` oplog entry.

Simultaneous index builds are resilient to replica set state transitions. The node that starts an
index build does not need to be the same node that decides to commit it.

See [Index Builds in Replicated Environments - MongoDB
Manual](https://docs.mongodb.com/master/core/index-creation/#index-builds-in-replicated-environments).

Server 7.1 introduces the following improvements:

- Index builds abort immediately after detecting errors other than duplicate key violations. Before
  7.1, index builds aborted the index build close to completion, potentially long after detection.
- A secondary member can abort a two-phase index build. Before 7.1, a secondary was forced to crash
  instead. See the [Voting for Abort](#voting-for-abort) section.
- Index builds are cancelled if there isn't enough storage space available. See the [Disk
  Space](#disk-space) section.

### Commit Quorum

The purpose of `commitQuorum` is to ensure secondaries are ready to commit an index build quickly.
This minimizes replication lag on secondaries: secondaries, on receipt of a `commitIndexBuild` oplog
entry, will stall oplog application until the local index build can be committed. `commitQuorum`
delays commit of an index build on the primary node until secondaries are also ready to commit. A
primary will not commit an index build until a minimum number of data-bearing nodes are ready to
commit the index build. Index builds can take anywhere from moments to days to complete, so the
replication lag can be very significant. Note: `commitQuorum` makes no guarantee that indexes on
secondaries are ready for use when the command completes, `writeConcern` must still be used for
that.

A `commitQuorum` option can be provided to the `createIndexes` command and specifies the number of
nodes, including itself, for which a primary must wait to be ready before committing. The
`commitQuorum` option accepts the same range of values as the writeConcern `"w"` option. This can be
an integer specifying the number of nodes, `"majority"`, `"votingMembers"`, or a replica set tag.
The default value is `"votingMembers"`, or all voting data-bearing nodes.

Nodes (both primary and secondary) submit votes to the primary when they have finished scanning all
data on a collection and performed the first drain of side-writes. Voting is implemented by a
`voteCommitIndexBuild` command, and is persisted as a write to the replicated
`config.system.indexBuilds` collection.

While waiting for a commit decision, primaries and secondaries continue receiving and applying new
side writes. When a quorum is reached, the current primary, under a collection X lock, will check
the remaining index constraints. If there are errors, it will replicate an `abortIndexBuild` oplog
entry. If the index build is successful, it will replicate a `commitIndexBuild` oplog entry.

Secondaries that were not included in the commit quorum and receive a `commitIndexBuild` oplog entry
will block replication until their index build is complete.

The `commitQuorum` for a running index build may be changed by the user via the
[`setIndexCommitQuorum`](https://github.com/mongodb/mongo/blob/v6.0/src/mongo/db/commands/commit_quorum/set_index_commit_quorum_command.cpp#L55)
server command.

See
[IndexBuildsCoordinator::\_waitForNextIndexBuildActionAndCommit](https://github.com/mongodb/mongo/blob/r4.4.0-rc9/src/mongo/db/index_builds_coordinator_mongod.cpp#L632).

### Voting for Abort

As of 7.1, a secondary can abort a two-phase index build by sending a `voteAbortIndexBuild` signal
to the primary. In contrast, before 7.1 it was forced to crash. Common causes for aborting the index
build are a killOp on the index build or running low on storage space. The primary, upon receiving a
vote to abort the index build from a secondary, will replicate an `abortIndexBuild` oplog entry.
This will cause all secondaries to gracefully abort the index build, even if a specific secondary
had already voted to commit the index build.

Note that once a secondary has voted to commit the index build, it cannot retract the vote. In the
unlikely event that a secondary has voted for commit and for some reason it must abort while waiting
for the primary to replicate a `commitIndexBuild` oplog entry, the secondary is forced to crash.

### Disk Space

As of 7.1, an index build can abort due to a replica set member running low on disk space. This
applies both to primary and secondary nodes. Additionally, on a primary the index build won't start
if the available disk space is low. The minimum amount of disk space is controlled by
[indexBuildMinAvailableDiskSpaceMB](https://github.com/mongodb/mongo/blob/406e69f6f5dee8b698c4e4308de2e9e5cef6c12c/src/mongo/db/storage/two_phase_index_build_knobs.idl#L71)
which defaults to 500MB.

## Resumable Index Builds

On clean shutdown, index builds save their progress in internal idents that will be used for
resuming the index builds when the server starts up. The persisted information includes:

- [Phase of the index
  build](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/resumable_index_builds.idl#L43)
  when it was interrupted for shutdown:
  - initialized
  - collection scan
  - bulk load
  - drain writes
- Information relevant to the phase for reconstructing the internal state of the index build at
  startup. This may include:
  - The internal state of the external sorter.
  - Idents for side writes, duplicate keys, and skipped records.

During [startup recovery](../storgae/README.md#startup-recovery), the persisted information is used
to reconstruct the in-memory state for the index build and resume from the phase that we left off
in. If we fail to resume the index build for whatever reason, the index build will restart from the
beginning.

Not all incomplete index builds are resumable upon restart. The current criteria for index build
resumability can be found in
[IndexBuildsCoordinator::isIndexBuildResumable()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/index_builds_coordinator.cpp#L375).
Generally, index builds are resumable under the following conditions:

- Storage engine is configured to be persistent with encryption disabled.
- The index build is running on a voting member of the replica set with the default [commit
  quorum](#commit-quorum) `"votingMembers"`.
- Majority read concern is enabled.

The [Recover To A Timestamp (RTT) rollback
algorithm](https://github.com/mongodb/mongo/blob/04b12743cbdcfea11b339e6ad21fc24dec8f6539/src/mongo/db/repl/README.md#rollback)
supports resuming index builds interrupted at any phase. On entering rollback, the resumable index
information is persisted to disk using the same mechanism as shutdown. We resume the index build
using the startup recovery logic that RTT uses to bring the node back to a writable state.

For improved rollback semantics, resumable index builds require a majority read cursor during
collection scan phase. Index builds wait for the majority commit point to advance before starting
the collection scan. The majority wait happens after installing the [side table for intercepting new
writes](#temporary-side-table-for-new-writes).

See
[MultiIndexBlock::\_constructStateObject()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/catalog/multi_index_block.cpp#L900)
for where we persist the relevant information necessary to resume the index build at shutdown and
[StorageEngineImpl::\_handleInternalIdents()](https://github.com/mongodb/mongo/blob/0d45dd9d7ba9d3a1557217a998ad31c68a897d47/src/mongo/db/storage/storage_engine_impl.cpp#L329)
for where we search for and parse the resume information on startup.

## Single-Phase Index Builds

Index builds on empty collections replicate a `createIndexes` oplog entry. This oplog entry was used
before FCV 4.4 for all index builds, but continues to be used in FCV 4.4 only for index builds that
are considered "single-phase" and do not need to run in the background. Unlike two-phase index
builds, the `createIndexes` oplog entry is always applied synchronously on secondaries during batch
application.

See
[createIndexForApplyOps](https://github.com/mongodb/mongo/blob/6ea7d1923619b600ea0f16d7ea6e82369f288fd4/src/mongo/db/repl/oplog.cpp#L176-L183).

## Memory Limits

The maximum amount of memory allowed for an index build is controlled by the
`maxIndexBuildMemoryUsageMegabytes` server parameter. The sorter is passed this value and uses it to
regulate when to write a chunk of sorted data out to disk in a temporary file. The sorter keeps
track of the chunks of data spilled to disk using one Iterator for each spill. The memory needed for
the iterators is taken out of the `maxIndexBuildMemoryUsageMegabytes` and it is a percentage of
`maxIndexBuildMemoryUsageMegabytes` define by the `maxIteratorsMemoryUsagePercentage` server
parameter with minimum value enough to store one iterator and maximum value 1MB.
