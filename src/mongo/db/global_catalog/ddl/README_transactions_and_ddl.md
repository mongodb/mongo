# Sharded Transactions and DDLs

This guide describes the consistency protocols specific to multi-statement transactions in the presence
of DDLs. Refer to [this architecture guide](https://github.com/mongodb/mongo/blob/8a79395deff895f18b8878ff4567c9fb309a7c64/src/mongo/db/s/README_sessions_and_transactions.md#transactions) for more general information about MongoDB multi-statement transactions.

In a sharded cluster, a multi-statement transaction (also referred to as a transaction)
establishes a storage snapshot on a participant shard when the first statement targets the shard.
The snapshot remains open and does not advance for the lifetime of the transaction. Two-phase
locking prevents DDLs from committing on the namespaces involved in the transaction until the
transaction commits or aborts. DDL in this context refers to both schema and data distribution
changes.

From a storage snapshot perspective, transactions behave as follows, depending on the read
concern level:

- "local" and "majority" read concern establish snapshots on each participant shards at an
  arbitrary timestamp that is not guaranteed to be the same across all shards.
- "snapshot" read concern establishes snapshots on participant shards at the same timestamp.

Participant shards validate the data ownership of the received statements, and reject requests that
use stale routing information, via the [placement versioning protocol](https://github.com/mongodb/mongo/blob/8a79395deff895f18b8878ff4567c9fb309a7c64/src/mongo/db/s/README_versioning_protocols.md).
Routers operate with a cached version of the routing table, which can be stale, and is lazily
refreshed when a shard informs the router that its table is stale. The routers refresh to the
latest version of the routing table.

From an isolation perspective:

- The data set in a transaction-participant shard operates with _snapshot_ isolation: any data
  distribution change occurring after the snapshot is established is invisible.
- The placement versioning protocol, and more broadly, the routing protocol, operate with _read
  committed_ level: routing continuously observe data distribution changes. This routing protocol
  behavior is also true on the shard end: the shard validates that the incoming request originates from
  a router that has the last data ownership information, regardless of whether this ownership
  information is visible in the existing data snapshot.

In practical terms, the routing protocol covers the case where the router is stale compared to the
shard's view of the catalog, however it is not designed to address the case where the router uses
information that is newer than (and incompatible with) the shard's snapshot. The routing protocol
in itself cannot forbid the following anomalies:

- **Data placement anomaly:** The router forwards a request to a shard using data ownership
  information that is newer than (and invisible in) the shard's snapshot. The shard's data snapshot is
  unable to observe the incoming range. Processing this request would miss data belonging to that
  range. This anomaly could occur when a range migration interleaves with an uncommitted transaction.
- **Collection generation anomaly:** The router forwards a request to the shard relative to a
  [collection generation](https://github.com/mongodb/mongo/blob/master/src/mongo/db/local_catalog/README_terminology.md)
  that is newer than the one in the shard's snapshot. This could occur, for instance, when the
  collection's namespace is recreated on the shard after the transaction has established a snapshot.
- **Collection incarnation anomaly:** Similar to the collection generation anomaly, but concerning
  the local catalog. The router forwards a request with ShardVersion::UNSHARDED, bypassing collection
  generation checks. The request might be for a namespace that was sharded when the transaction
  established the snapshot. Processing this request would incorrectly return partial data for the
  collection as the router only targeted the primary shard.

The sections below describe the protocols transactions use along with the placement versioning
protocol to forbid the anomalies above.

## Transactions with readConcern="snapshot"

For readConcern: "snapshot" transactions, only the collection generation anomaly is applicable.

On the first statement, the router chooses an `atClusterTime` (i.e., it selects its latest known
`VectorClock::clusterTime`), unless the transaction specifies one.

For **tracked** collections, the protocol is as follows:

1. The router forwards statements using its latest routing table, but interprets it as of
   `atClusterTime` by consulting the [chunk ownership history](https://github.com/mongodb/mongo/blob/6afc3207668d5dca4e7168bdb089f74bc299ef06/src/mongo/s/catalog/type_chunk.h#L295-L296).
1. The targeted shard checks the attached placement version. All the following conditions must be
   met for the request to be considered valid:
   1. It must match the current (latest) placement version for this shard.
   1. The received `atClusterTime` must not be earlier than the latest placement version's [timestamp field](https://github.com/mongodb/mongo/blob/8a79395deff895f18b8878ff4567c9fb309a7c64/src/mongo/db/s/README_versioning_protocols.md#shard-version) known by the shard.
      This field represents the commit timestamp of the latest collection generation operation
      (e.g. shardCollection, renameCollection, etc) on this sharded collection.

Notes:

- (2.i) ensures that the routing table used by the router (including its history) is not stale. This
  is part of the placement versioning protocol.
- (2.ii) ensures that the collection did not undergo any collection generation change at a timestamp
  later than `atClusterTime`, which would make the current routing/filtering metadata invalid to be used
  with the point-in-time storage snapshot. This proscribes the collection generation anomaly.

For **untracked** collections, the protocol is as follows:

1. The router forwards statements using the latest database version, and targets its primary shard.
   ShardVersion::UNSHARDED is attached in addition to the DatabaseVersion.
1. The targeted shard checks the attached metadata. All the following conditions must be
   met for the request to be considered valid:
   1. The received database version must match the current (latest) database version.
   1. The received `atClusterTime` must not be earlier than the latest database version's [timestamp field](https://github.com/mongodb/mongo/blob/eeef1763cb0ff77757bb60eabb8ad1233c990786/src/mongo/db/s/README_versioning_protocols.md#database-version) known by the shard.
      This field represents the commit timestamp of the latest reincarnation (drop/create) or movePrimary operation for this database.
   1. The received placement version is UNSHARDED, and the shard checks the latest version matches.
   1. The collection in the snapshot must be the same incarnation (same UUID) as in the latest CollectionCatalog.

Notes:

- (2.i) ensures that the router's database primary shard knowledge is not stale. This is part of the
  database versioning protocol.
- (2.ii) ensures that the database did not undergo any reincarnation at a timestamp later than
  `atClusterTime`. The router always routes requests for untracked collections based on the latest
  database primary shard knowledge, but this decision might not be valid at the specified cluster time.
  E.g. if the shard was not the primary shard for the database at that point in time.
- (2.iii) ensures that the collection generation anomaly is detected for cases where an untracked
  collection becomes tracked. There will be a mismatch between the attached ShardVersion::UNSHARDED
  and the actual placement version on the shard.
- (2.iv) ensures that the collection incarnation anomaly is detected by the primary shard after a
  sharded collection is reincarnated as unsharded (by definition, ShardVersion::UNSHARDED always conforms with 2.ii).

## Transactions with readConcern="local" or "majority"

This protocol is more complex, because with read concern levels weaker than "snapshot", each
participant shard can open their read snapshot at different timestamps, and the router is unaware
of the timestamp they chose. Both data placement and collection generation anomalies apply.

On the first statement, the router chooses a `placementConflictTime` (i.e., it selects its latest known
`VectorClock::clusterTime`) at the beginning of the transaction and uses the same `placementConflictTime` for all statements.

For **tracked** collections, the protocol is as follows:

1. For each statement, the router uses the latest routing table (considers the latest version).
1. For each statement, the router sends the command to the targeted shard. It attaches the placement
   version as usual, and additionally attaches the selected `placementConflictTime`. It also
   attaches an `afterClusterTime` = `placementConflictTime`.
1. The targeted shard will open its storage snapshot with a timestamp at least `afterClusterTime`.
1. The targeted shard checks the attached placement version. All the following conditions must be
   met for the request to be considered valid:
   1. It must match the current (latest) placement version for this shard.
   1. `placementConflictTime` must not be earlier than the placement version's [timestamp field](https://github.com/mongodb/mongo/blob/8a79395deff895f18b8878ff4567c9fb309a7c64/src/mongo/db/s/README_versioning_protocols.md#shard-version).
      This field represents the commit timestamp of the latest DDL operation (e.g. create, rename,
      etc) on this collection.
   1. `placementConflictTime` must not be earlier than the latest _incoming_ migration commit timestamp on this
      shard for this collection.

Notes:

- (4.i) ensures that the routing table used by the router is not stale. This is part of the placement
  versioning protocol.
- (4.ii) ensures that the collection did not undergo any collection generation change at a timestamp
  later than `placementConflictTime`, which would make the current routing/filtering metadata
  invalid to be used with the open snapshot. This proscribes the collection generation anomaly.
- (4.iii) ensures no migration committed since `placementConflictTime`. This proscribes the data
  placement anomaly.
- The `afterClusterTime` selected at (2) imposes a lower bound for each shard's snapshot read
  timestamp. The (4.i) and (4.ii) assertions check that the metadata/placement has not changed since
  that lower bound, therefore guaranteeing that the assertions are valid for whatever timestamp could
  have been ultimately selected. The lower bound imposed by `afterClusterTime` is necessary because
  there is otherwise no guarantee that the shard would open a snapshot that is at least inclusive of
  the `placementConflictTime`. Consider this scenario involving a readConcern:"majority" transaction:
  - The config server commits the range migration at timestamp T100, and majority replicates it.
  - The shard learns the commit timestamp from the config server, sets its latest migration commit
    timestamp to T100, and rescinds the critical section.
  - The first transaction statement comes in, with `placementConflictTime=T100`. If the
    shard's majority commit point has not yet advanced to T100, in the absence of `afterClusterTime=T100`,
    the shard could open a snapshot at T99, and miss the incoming range.
- By design, it is not possible for a router to cache routing information accounting for the
  latest migration, without having gossiped in a clusterTime that is at least as recent as that
  migration's commit timestamp.

For **untracked** collections, the protocol is as follows:

1. For each statement, the router uses the latest database version.
1. For each statement, the router sends the command to the database primary shard. It attaches the database
   version as usual, and additionally attaches the selected `placementConflictTime`. It also
   attaches an `afterClusterTime` = `placementConflictTime`, and ShardVersion::UNSHARDED.
1. The targeted shard will open its storage snapshot with a timestamp at least `afterClusterTime`.
1. The targeted shard checks the attached metadata. All the following conditions must be
   met for the request to be considered valid:
   1. The received database version must match the current (latest) database version.
   1. `placementConflictTime` must not be earlier than the database version's [timestamp field](https://github.com/mongodb/mongo/blob/eeef1763cb0ff77757bb60eabb8ad1233c990786/src/mongo/db/s/README_versioning_protocols.md#database-version).
      This field represents the commit timestamp of the latest reincarnation (drop/create) or movePrimary operation for this database.
   1. The received placement version is UNSHARDED, and the shard checks the latest version matches.
   1. The collection in the snapshot must be the same incarnation (same UUID) as in the latest CollectionCatalog.

Notes:

- (4.i) ensures that the database version used by the router is not stale. This is part of the database
  versioning protocol.
- (4.ii) ensures that the database did not undergo any reincarnation at a timestamp later than
  `placementConflictTime`. The router always routes requests for untracked collections based on the latest
  database primary shard knowledge, but this decision might not be valid at for snapshots opened at
  a timestamp before the reincarnation.
- (4.iii) ensures that the collection generation anomaly is detected for cases where an untracked
  collection becomes tracked. There will be a mismatch between the attached ShardVersion::UNSHARDED
  and the actual placement version on the shard.
- (4.iv) ensures that the collection incarnation anomaly is detected by the primary shard after a
  sharded collection is reincarnated unsharded.

A formal specification of the placement versioning protocol and the protocol avoiding the data
placement anomaly is available [here](/src/mongo/tla_plus/TxnsMoveRange).

A formal specification of the protocol avoiding the collection generation and collection incarnation
anomalies is available [here](/src/mongo/tla_plus/TxnsCollectionIncarnation).
