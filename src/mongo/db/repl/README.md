# Replication Internals

Replication is the set of systems used to continuously copy data from a primary server to secondary
servers so if the primary server fails a secondary server can take over soon. This process is
intended to be mostly transparent to the user, with drivers taking care of routing queries to the
requested replica. Replication in MongoDB is facilitated through [**replica
sets**](https://docs.mongodb.com/manual/replication/).

Replica sets are a group of nodes with one primary and multiple secondaries. The primary is
responsible for all writes. Users may specify that reads from secondaries are acceptable via
[`setSecondaryOk`](https://docs.mongodb.com/manual/reference/method/Mongo.setSecondaryOk/) or through
[**read preference**](#read-preference), but they are not by default.

# Steady State Replication

The normal running of a replica set is referred to as steady state replication. This is when there
is one primary and multiple secondaries. Each secondary is replicating data from the primary, or
another secondary off of which it is **chaining**.

## Life as a Primary

### Doing a Write

When a user does a write, all a primary node does is apply the write to the database like a
standalone would. The one difference from a standalone write is that replica set nodes have an
`OpObserver` that inserts a document to the **oplog** whenever a write to the database happens,
describing the write. The oplog is a capped collection called `oplog.rs` in the `local` database.
There are a few optimizations made for it in WiredTiger, and it is the only collection that doesn't
include an \_id field.

If a write does multiple operations, each will have its own oplog entry; for example, inserts with
implicit collection creation create two oplog entries, one for the `create` and one for the
`insert`.

These entries are rewritten from the initial operation to make them idempotent; for example, updates
with `$inc` are changed to use `$set`.

Secondaries drive oplog replication via a pull process.

Writes can also specify a [**write
concern**](https://docs.mongodb.com/manual/reference/write-concern/). If a command includes a write
concern, the command will just block in its own thread until the oplog entries it generates have
been replicated to the requested number of nodes. The primary keeps track of how up-to-date the
secondaries are to know when to return. A write concern can specify a number of nodes to wait for,
or **majority**. If **majority** is specified, the write waits for that write to be in the
**committed snapshot** as well, so that it can be read with `readConcern: { level: majority }`
reads. (If this last sentence made no sense, come back to it at the end).

### Default Write Concern

If a write operation does not explicitly specify a write concern, the server will use a default
write concern. This default write concern will be defined by either the
**cluster-wide write concern**, explicitly set by the user, or the
**implicit default write concern**, implicitly set by the
server based on replica set configuration.

#### Cluster-Wide Write Concern

Users can set the cluster-wide write concern (CWWC) using the
[`setDefaultRWConcern`](https://docs.mongodb.com/manual/reference/command/setDefaultRWConcern/)
command. Setting the CWWC will cause the implicit default write concern to
no longer take effect. Once a user sets a CWWC, we disallow unsetting it. The reasoning
behind this is explored in the section
[Implicit Default Write Concern and Sharded Clusters](#implicit-default-write-concern-and-sharded-clusters).

On sharded clusters, the CWWC will be stored on config servers. Shard servers themselves do not
store the CWWC. Instead, mongos polls the config server and applies the default write concern to
requests it forwards to shards.

#### Implicit Default Write Concern

If there is no cluster-wide default write concern set, the server will set the default. This is
known as the implicit default write concern (IDWC). For most cases, the IDWC will default to
`{w: "majority")`.

The IDWC is calculated on startup using the **Default Write Concern Formula (DWCF)**:

`implicitDefaultWriteConcern = if ((#arbiters > 0) AND (#non-arbiters <= majority(#voting nodes)) then {w:1} else {w:majority}`

This formula specifies that for replica sets with arbiters, we want to ensure that we set the
implicit default to a value that the set can satisfy in the event of one data-bearing node
going down. That is, the number of data-bearing nodes must be strictly greater than the majority
of voting nodes for the set to set `{w: "majority"}`.

For example, if we have a PSA replica set, and the secondary goes down, the primary cannot
successfully acknowledge a majority write as the majority for the set is two nodes. However, the
primary will remain primary with the arbiter's vote. In this case, the DWCF will have preemptively
set the IDWC to `{w: 1}` so the user can still perform writes to the replica set.

#### Implicit Default Write Concern and Sharded Clusters

For sharded clusters, the implicit default write concern will always be `{w: "majority"}`.
As mentioned above, mongos will send the default write concern with all requests that it forwards
to shards, which means the default write concern on shards will always be consistent in the
cluster. We don't want to specify `{w: "majority"}` for shard replica sets
that can keep a primary due to an arbiter's vote, but lose the ability to acknowledge majority
writes if a majority of data-bearing nodes goes down. So if the result of the DWCF for any replica
set in the cluster is `{w: 1}`, we require the cluster to set a CWWC. Once set, we disallow
unsetting it so we can prevent PSA shards from implicitly defaulting to `{w: "majority"}` for
reasons mentioned above. However, if a user decides to set the CWWC to `{w: "majority"}`
for a PSA set, they may do so. We assume that in this case the user understands
the tradeoffs they are making.

We will fassert shard servers on startup if no CWWC is set and
the result of the default write concern formula is `{w: 1}`. Similarly, we will also fail any
`addShard` command that attempts to add a shard replica set with a default write concern of
`{w: 1}` when CWWC is unset. This is because we want to maintain a consistent implicit default of
`{w: "majority"}` across the cluster, but we do not want to specify that for PSA sets for reasons
listed above.

#### Replica Set Reconfigs and Default Write Concern

A replica set reconfig will recalculate the default write concern using the Default Write Concern
Formula if CWWC is not set. If the new value of the implicit default write concern is different
from the old value, we will fail the reconfig. Users must set a CWWC before issuing a reconfig
that would change the IDWC.

#### Force Reconfigs

As an important note, we will also fail force reconfigs that may change
the IDWC. In cases where a replica set is facing degraded performance and cannot satisfy a
majority write concern needed to set the CWWC, users can run
`setDefaultRWConcern` with write concern `{w: 1}` instead of making it a majority write so that
setting CWWC does not get in the way of being able to do a force reconfig.

#### Code References

- [The definition of an Oplog Entry](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/oplog_entry.idl)
- [Upper layer uses OpObserver class to write Oplog](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/op_observer/op_observer.h#L112), for example, [it is helpful to take a look at ObObserverImpl::logOperation()](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/op_observer/op_observer_impl.cpp#L114)
- [repl::logOplogRecords() is a common function to write Oplogs into Oplog Collection](https://github.com/mongodb/mongo/blob/r7.1.0/src/mongo/db/repl/oplog.cpp#L440)
- [WriteConcernOptions is filled in extractWriteConcern()](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/write_concern.cpp#L71)
- [Upper level uses waitForWriteConcern() to wait for the write concern to be fulfilled](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/write_concern.cpp#L254)

## Life as a Secondary

In general, secondaries just choose a node to sync from, their **sync source**, and then pull
operations from its oplog and apply those oplog entries to their own copy of the data on disk.

Secondaries also constantly update their sync source with their progress so that the primary can
satisfy write concerns.

### Oplog Fetching

A secondary keeps its data synchronized with its sync source by fetching oplog entries from its sync
source. This is done via the
[`OplogFetcher`](https://github.com/mongodb/mongo/blob/929cd5af6623bb72f05d3364942e84d053ddea0d/src/mongo/db/repl/oplog_fetcher.h) which
runs in its own separate thread, communicating via [a dedicated connection (DBClientConnection)](https://github.com/mongodb/mongo/blob/90e4270e9b22071c7d0367195c56bf2c5b50e56f/src/mongo/db/repl/oplog_fetcher.cpp#L190) with that instance.

The `OplogFetcher` does not directly apply the operations it retrieves from the sync source.
Rather, it puts them into a buffer (the **`OplogBuffer`**) and another thread is in charge of
taking the operations off the buffer and applying them. That buffer uses an in-memory blocking
queue for steady state replication; there is a similar collection-backed buffer used for initial
sync.

#### Oplog Fetcher Lifecycle

The `OplogFetcher` is owned by the
[`BackgroundSync`](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/bgsync.h) thread.
The `BackgroundSync` thread runs continuously while a node is in `SECONDARY` state.
`BackgroundSync` sits in a loop, where each iteration it first chooses a sync source with the
`SyncSourceResolver` and then starts up the `OplogFetcher`.

In steady state, the `OplogFetcher` continuously receives and processes batches of oplog entries
from its sync source.

The `OplogFetcher` could terminate because the first batch implies that a rollback is required, it
could receive an error from the sync source, or it could just be shut down by its owner, such as
when `BackgroundSync` itself is shut down. In addition, after every batch, the `OplogFetcher` runs
validation checks on the documents in that batch. It then decides if it should continue syncing
from the current sync source. If validation fails, or if the node decides to stop syncing, the
`OplogFetcher` will shut down.

When the `OplogFetcher` terminates, `BackgroundSync` restarts sync source selection, exits, or goes
into ROLLBACK depending on the return status.

#### Oplog Fetcher Implementation Details

Let’s refer to the sync source as node A and the fetching node as node B.

After starting up, the `OplogFetcher` first creates a connection to sync source A. Through this
connection, it will establish an **exhaust cursor** to fetch oplog entries. This means that after
the initial `find` and `getMore` are sent, A will keep sending all subsequent batches without
needing B to run any additional `getMore`s.

The `find` command that B’s `OplogFetcher` first sends to sync source A has a greater than or equal
predicate on the timestamp of the last oplog entry it has fetched. The original `find` command
should always return at least 1 document due to the greater than or equal predicate. If it does
not, that means that A’s oplog is behind B's and thus A should not be B’s sync source. If it does
return a non-empty batch, but the first document returned does not match the last entry in B’s
oplog, there are two possibilities. If the oldest entry in A's oplog is newer than B's latest
entry, that means that B is too stale to sync from A. As a result, B denylists A as a sync source
candidate. Otherwise, B's oplog has diverged from A's and it should go into
[**ROLLBACK**](https://docs.mongodb.com/manual/core/replica-set-rollbacks/).

After getting the original `find` response, secondaries check the metadata that accompanies the
response to see if the sync source is still a good sync source. Secondaries check that the node has
not rolled back since it was chosen and that it is still ahead of them.

The `OplogFetcher` specifies `awaitData: true, tailable: true` on the cursor so that subsequent
batches block until their `maxTimeMS` expires waiting for more data instead of returning
immediately. If there is no data to return at the end of `maxTimeMS`, the `OplogFetcher` receives
an empty batch and will wait on the next batch.

If the `OplogFetcher` encounters any errors while trying to connect to the sync source or get a
batch, it will use `OplogFetcherRestartDecision` to check that it has enough retries left to create
a new cursor. The connection class will automatically handle reconnecting to the sync source when
needed. Whenever the `OplogFetcher` successfully receives a batch, it will reset its retries. If it
errors enough times in a row to exhaust its retries, that might be an indication that there is
something wrong with the connection or the sync source. In that case, the `OplogFetcher` will shut
down with an error status.

The `OplogFetcher` may shut down for a variety of other reasons as well. After each successful
batch, the `OplogFetcher` decides if it should continue syncing from the current sync source. If
the `OplogFetcher` decides to continue, it will wait for the next batch to arrive and repeat. If
not, the `OplogFetcher` will terminate, which will lead to `BackgroundSync` choosing a new sync
source. Reasons for changing sync sources include:

- If the node is no longer in the replica set configuration.
- If the current sync source is no longer in the replica set configuration.
- If the user has requested another sync source via the `replSetSyncFrom` command.
- If chaining is disabled and the node is not currently syncing from the primary.
- If the sync source is not the primary, does not have its own sync source, and is not ahead of
  the node. This indicates that the sync source will not receive writes in a timely manner. As a
  result, continuing to sync from it will likely cause the node to be lagged.
- If the most recent OpTime of the sync source is more than `maxSyncSourceLagSecs` seconds behind
  another member's latest oplog entry. This ensures that the sync source is not too far behind
  other nodes in the set. `maxSyncSourceLagSecs` is a server parameter and has a default value of
  30 seconds.
- If the node has discovered another eligible sync source that is significantly closer. A
  significantly closer node has a ping time that is at least `changeSyncSourceThresholdMillis`
  lower than our current sync source. This minimizes the number of nodes that have sync sources
  located far away.`changeSyncSourceThresholdMillis` is a server parameter and has a default value
  of 5 ms.

### Sync Source Selection

Whenever a node starts initial sync, creates a new `BackgroundSync` (when it stops being primary),
or errors on its current `OplogFetcher`, it must get a new sync source. Sync source selection is
done by the
[`SyncSourceResolver`](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/sync_source_resolver.h).

The `SyncSourceResolver` delegates the duty of choosing a "sync source candidate" to the
[**`ReplicationCoordinator`**](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator.h),
which in turn asks the
[**`TopologyCoordinator`**](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/topology_coordinator.h)
to choose a new sync source.

#### Choosing a sync source candidate

To choose a new sync source candidate, the `TopologyCoordinator` first checks if the user requested
a specific sync source with the `replSetSyncFrom` command. In that case, the secondary chooses that
host as the sync source and resets its state so that it doesn’t use that requested sync source
again.

If **chaining** is disallowed, the secondary needs to sync from the primary, and chooses it as a
candidate.

Otherwise, it iterates through all of the nodes and sees which one is the best.

- First the secondary checks the `TopologyCoordinator`'s cached view of the replica set for the
  latest OpTime known to be on the primary. Secondaries do not sync from nodes whose newest oplog
  entry is more than
  [`maxSyncSourceLagSecs`](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/topology_coordinator.cpp#L302-L315)
  seconds behind the primary's newest oplog entry.
- Secondaries then loop through each node and choose the closest node that satisfies [various
  criteria](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/topology_coordinator.cpp#L200-L438).
  “Closest” here is determined by the lowest ping time to each node.
- If no node satisfies the necessary criteria, then the `BackgroundSync` waits 1 second and restarts
  the sync source selection process.

#### Sync Source Probing

After choosing a sync source candidate, the `SyncSourceResolver` probes the sync source candidate to
make sure it actually is able to fetch from the sync source candidate’s oplog.

- If the sync source candidate has no oplog or there is an error, the secondary denylists that sync
  source for some time and then tries to find a new sync source candidate.
- If the oldest entry in the sync source candidate's oplog is newer than the node's newest entry,
  then the node denylists that sync source candidate as well because the candidate is too far
  ahead.
- The sync source's **RollbackID** is also fetched to be checked after the first batch is returned
  by the `OplogFetcher`.

If the secondary is too far behind all possible sync source candidates then it goes into maintenance
mode and waits for manual intervention (likely a call to `resync`). If no viable candidates were
found, `BackgroundSync` waits 1 second and attempts the entire sync source selection process again.
Otherwise, the secondary found a sync source! At that point `BackgroundSync` starts an OplogFetcher.

### Oplog Entry Persistence

There is a dedicated thread called [`OplogWriter`](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/oplog_writer.h#L44)
to write the fetched oplog entries into `rs.oplog` collection and trigger journal flush to persist
those oplog entries.

The `OplogWriter` runs in an endless loop doing the followings:

1. Get a batch from the writer batcher, which is encapsulated in the [`OplogWriterBatcher`](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/oplog_writer_batcher.cpp#L60).
2. Write the batch of oplog entries into the oplog.
3. Update [**oplog visibility**](../catalog/README.md#oplog-visibility) by notifying the storage
   engine of the new oplog entries.
4. Advance the node's `lastWritten` optime to the last optime in the batch.
5. Tell the storage engine to flush the journal.
6. Push the written oplog batches to the OplogApplier's buffer.

### Oplog Entry Application

A separate thread, `ReplBatcher`, runs the
[`OplogApplierBatcher`](https://github.com/mongodb/mongo/blob/r8.0.0-rc2/src/mongo/db/repl/oplog_applier_batcher.h)
and is used for pulling oplog entries off of the oplog applier buffer and creating the next batch
that will be applied. These batches are called **oplog applier batches** and are different from
**oplog fetcher batches**, which are sent by a node's sync source during [oplog fetching](#oplog-fetching).
Oplog applier batches differ from oplog fetcher batches because they have more restrictions than
just size limits when creating a new batch. Operations in a batch are applied in parallel when
possible, so there are certain operation types (like commands) which require being in their own
oplog applier batch. For example, a `dropDatabase` operation shouldn't be applied in parallel with
other operations, so it must be in a batch of size one.

The
[`OplogApplier`](https://github.com/mongodb/mongo/blob/r8.0.0-rc2/src/mongo/db/repl/oplog_applier.h)
is in charge of applying each batch of oplog entries received from the batcher. It will run in an
endless loop doing the following:

1. Get the next oplog applier batch from the batcher.
2. Use multiple threads to apply the batch in parallel. This means that oplog entries within the
   same batch are not necessarily applied in order. The operations in each batch will be divided
   among the writer threads. The only restriction for creating the vector of operations that each
   writer thread will apply serially has to do with the documents that the operation applies to.
   Operations on a document must be atomic and ordered, and are hence put on the same thread to be
   serialized. Operations on the same collection can still be parallelized if they are working with
   distinct documents. When applying operations, each writer thread will try to **group** together
   insert operations for improved performance and will apply all other operations individually.
3. Finalize the batch by advancing the global timestamp (and the node's lastApplied optime) to the
   last optime in the batch.

#### Code References

- [Start background threads like bgSync/oplogApplier/syncSourceFeedback](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L252)
- [BackgroundSync starts SyncSourceResolver and OplogFetcher to sync log](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/bgsync.cpp#L225)
- [SyncSourceResolver chooses a sync source to sync from](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/sync_source_resolver.cpp#L545)
- [OplogBuffer currently uses a BlockingQueue as underlying data structure](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/oplog_buffer_blocking_queue.h#L41)
- [OplogFetcher queries from sync source and put fetched oplogs in OplogApplier::\_oplogBuffer](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/oplog_fetcher.cpp#L209)
- [OplogWriterBatcher merges oplog entries from the oplog writer buffer to create a batch to write](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/oplog_writer_batcher.cpp#L80)
- [OplogWriter writes down the oplog entries batched by OplogWriterBatcher](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/oplog_writer_impl.cpp#L225)
- [OplogApplierBatcher polls oplogs from OplogApplier::\_oplogBuffer and creates an OplogBatch to apply](https://github.com/mongodb/mongo/blob/r8.0.0-rc2/src/mongo/db/repl/oplog_applier_batcher.cpp#L337)
- [OplogApplier gets batches of oplog entries from the OplogApplierBatcher and applies entries in parallel](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/oplog_applier_impl.cpp#L297)
- [SyncSourceFeedback keeps checking if there are new oplogs applied on this instance and issues `UpdatePositionCmd` to sync source](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/sync_source_feedback.cpp#L157)

## Replication and Topology Coordinators

The `ReplicationCoordinator` is the public api that replication presents to the rest of the code
base. It is in charge of coordinating the interaction of replication with the rest of the system.

The `ReplicationCoordinator` communicates with the storage layer and other nodes through the
[`ReplicationCoordinatorExternalState`](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_external_state.h).
The external state also manages and owns all of the replication threads.

The `TopologyCoordinator` is in charge of maintaining state about the topology of the cluster. On
significant changes (anything that affects the response to hello/isMaster), the TopologyCoordinator
updates its TopologyVersion. The [`hello`](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_info.cpp#L346) command awaits changes in the TopologyVersion before returning. On
shutdown, if the server is a secondary, it enters quiesce mode: we increment the TopologyVersion
and start responding to `hello` commands with a `ShutdownInProgress` error, so that clients cease
routing new operations to the node.

Since we wish to track usage of the `isMaster` command separately from the `hello` command in
`serverStatus`, it is implemented as a [derived class](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_info.cpp#L679) of hello. The main difference between the two commands is that
clients will start seeing an `isWritablePrimary` response field instead of `ismaster` when switching
to the `hello` command.

The `TopologyCoordinator` is non-blocking and does a large amount of a node's decision making
surrounding replication. Most replication command requests and responses are filled in here.

Both coordinators maintain views of the entire cluster and the state of each node, though there are
plans to merge these together.

## helloOk Protocol Negotiation

In order to preserve backwards compatibility with old drivers, we currently support both the
[`isMaster`](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_info.cpp#L679)
command and the [`hello`](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_info.cpp#L346) command. New drivers and 5.0+ versions of the server will support `hello`.
A new driver will send "helloOk: true" as a part of the initial handshake when opening a new
connection to mongod. If the server supports hello, it will respond with "helloOk: true" as well.
This way, new drivers know that they're communicating with a version of the server that supports
`hello` and will start sending `hello` instead of `isMaster` on this connection.

If the server does not support `hello`, the `helloOk` flag is ignored. A new driver will subsequently
not see "helloOk: true" in the response and continue to send `isMaster` on this connection. Old drivers
will not specify this flag at all, so the behavior remains the same.

Communication between nodes in the cluster is always done using `hello`, never with `isMaster`.

## Communication

Each node has a copy of the **`ReplicaSetConfig`** in the `ReplicationCoordinator` that lists all
nodes in the replica set. This config lets each node talk to every other node.

Each node uses the internal client, the legacy c++ driver code in the
[`src/mongo/client`](https://github.com/mongodb/mongo/tree/r4.2.0/src/mongo/client) directory, to
talk to each other node. Nodes talk to each other by sending a mixture of external and internal
commands over the same incoming port as user commands. All commands take the same code path as
normal user commands. For security, nodes use the keyfile to authenticate to each other. You need to
be the system user to run replication commands, so nodes authenticate as the system user when
issuing remote commands to other nodes.

Each node communicates with other nodes at regular intervals to:

- Check the liveness of the other nodes (heartbeats)
- Stay up to date with the primary (oplog fetching)
- Update their sync source with their progress (`replSetUpdatePosition` commands)

Each oplog entry is assigned a unique `OpTime` to describe when it occurred so other nodes can
compare how up-to-date they are.

OpTimes include a timestamp and a term field. The term field indicates how many elections have
occurred since the replica set started.

The election protocol, known as
[protocol version 1 or PV1](https://docs.mongodb.com/manual/reference/replica-set-protocol-versions/),
is built on top of [Raft](https://raft.github.io/raft.pdf), so it is guaranteed that two primaries
will not be elected in the same term. This helps differentiate ops that occurred at the same time
but from different primaries in the case of a network partition.

### Oplog Fetcher Responses

The `OplogFetcher` just issues normal `find` and `getMore` commands, so the upstream node (the sync
source) does not get any information from the request. In the response, however, the downstream
node, the one that issues the `find` to its sync source, gets metadata that it uses to update its
view of the replica set.

There are two types of metadata, `ReplSetMetadata` and `OplogQueryMetadata`. (The
`OplogQueryMetadata` is new, so there is some temporary field duplication for backwards
compatibility.)

#### ReplSetMetadata

`ReplSetMetadata` comes with all replication commands and is processed similarly for all commands.
It includes:

1. The upstream node's last committed OpTime
2. The current term.
3. The `ReplicaSetConfig` version and term (this is used to determine if a reconfig has occurred on
   the upstream node that hasn't been registered by the downstream node yet).
4. The replica set ID.
5. Whether the upstream node is primary.

The node sets its term to the upstream node's term, and if it's a primary (which can only happen on
heartbeats), it steps down.

The last committed OpTime is only used in this metadata for
[arbiters](https://docs.mongodb.com/manual/core/replica-set-arbiter/), to advance their committed
OpTime and in sharding in some places. Otherwise it is ignored.

#### OplogQueryMetadata

`OplogQueryMetadata` only comes with `OplogFetcher` responses. It includes:

1. The upstream node's last committed OpTime. This is the most recent operation that would be
   reflected in the snapshot used for `readConcern: majority` reads.
2. The upstream node's lastWritten and lastApplied OpTime.
3. The index (as specified by the `ReplicaSetConfig`) of the node that the upstream node thinks is
   primary.
4. The index of the upstream node's sync source.

If the metadata says there is still a primary, the downstream node resets its election timeout into
the future.

The downstream node sets its last committed OpTime to the last committed OpTime of the upstream
node.

When it updates the last committed OpTime, it chooses a new committed snapshot if possible and tells
the storage engine to erase any old ones if necessary.

Before sending the next `getMore`, the downstream node uses the metadata to check if it should
change sync sources.

### Heartbeats

At a default of every 2 seconds, the `HeartbeatInterval`, every node sends a heartbeat to every
other node with the `replSetHeartbeat` command. This means that the number of heartbeats increases
quadratically with the number of nodes and is the reasoning behind the
[50 member limit](https://github.com/mongodb/mongo/blob/r4.4.0-rc6/src/mongo/db/repl/repl_set_config.h#L133)
in a replica set. The data, `ReplSetHeartbeatArgsV1` that accompanies every heartbeat is:

1. `ReplicaSetConfig` version
2. `ReplicaSetConfig` term
3. The id of the sender in the `ReplSetConfig`
4. Term
5. Replica set name
6. Sender host address

When the remote node receives the heartbeat, it first processes the heartbeat data, and then sends a
response back. First, the remote node makes sure the heartbeat is compatible with its replica set
name. Otherwise it sends an error.

The receiving node's `TopologyCoordinator` updates the last time it received a heartbeat from the
sending node for liveness checking in its `MemberData` list.

If the sending node's config is newer than the receiving node's, then the receiving node schedules a
heartbeat to get the config, except when the receiving node is [in primary state but cannot accept
non-local writes](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L683-L691).
The receiving node's `TopologyCoordinator` also updates its `MemberData` with the last update from
the sending node and marks it as being up. See more details on config propagation via heartbeats in
the [Reconfiguration](#Reconfiguration) section.

It then creates a `ReplSetHeartbeatResponse` object. This includes:

1. Replica set name
2. The receiving node's election time
3. The receiving node's lastWritten OpTime
4. The receiving node's lastApplied OpTime
5. The receiving node's lastDurable OpTime
6. The term of the receiving node
7. The state of the receiving node
8. The receiving node's sync source
9. The receiving node's `ReplicaSetConfig` version and term
10. Whether the receiving node is primary
11. Whether the receiving node is electable

When the sending node receives the response to the heartbeat, it first processes its
`ReplSetMetadata` like before.

The sending node postpones its election timeout if it sees a primary.

The `TopologyCoordinator` updates its `MemberData`. It marks if the receiving node is up or down.

The sending node's `TopologyCoordinator` then looks at the response and decides the next action to
take: no action, priority takeover, or reconfig,

The `TopologyCoordinator` then updates the `MemberData` for the receiving node with its most
recently acquired OpTimes.

The next heartbeat is scheduled and then the next action set by the `TopologyCoordinator` is
executed.

If the action was a priority takeover, then the node ranks all of the priorities in its config and
assigns itself a priority takeover timeout proportional to its rank. After that timeout expires the
node will check if it's eligible to run for election and if so will begin an election. The timeout
is simply: `(election timeout) * (priority rank + 1)`.

Heartbeat threads belong to the
[`ReplCoordThreadPool`](https://github.com/mongodb/mongo/blob/674d57fc70d80dedbfd634ce00ca4b967ea89646/src/mongo/db/mongod_main.cpp#L944)
connection pool started by the
[`ReplicationCoordinator`](https://github.com/mongodb/mongo/blob/674d57fc70d80dedbfd634ce00ca4b967ea89646/src/mongo/db/mongod_main.cpp#L986).
Note that this connection pool is separate from the dedicated connection used by the [Oplog fetcher](#oplog-fetching).

### Commit Point Propagation

The replication majority **commit point** refers to an OpTime such that all oplog entries with an
OpTime earlier or equal to it have been replicated to a majority of nodes in the replica set. It is
influenced by the [`lastWritten`](#replication-timestamp-glossary) and the
[`lastDurable`](#replication-timestamp-glossary) OpTimes.

On the primary, we advance the commit point by checking what the highest `lastWritten` or
`lastDurable` is on a majority of the nodes. This OpTime must be greater than the current
`commit point` for the primary to advance it. Any threads blocking on a writeConcern are woken up
to check if they now fulfill their requested writeConcern.

When `getWriteConcernMajorityShouldJournal` is set to true, the
[`_lastCommittedOpTime`](#replication-timestamp-glossary) is set to the `lastDurable` OpTime. This
means that the server acknowledges a write operation after a majority has written to the on-disk
journal. Otherwise, `_lastCommittedOpTime` is set using the `lastWritten`.

Secondaries advance their commit point via heartbeats by checking if the commit point is in the
same term as their `lastWritten` OpTime. This ensures that the secondary is on the same branch of
history as the commit point. Additionally, they can update their commit point via the spanning tree
by taking the minimum of the learned commit point and their `lastWritten`.

### Update Position Commands

The last way that replica set nodes regularly communicate with each other is through
`replSetUpdatePosition` commands. The `ReplicationCoordinatorExternalState` creates a
[**`SyncSourceFeedback`**](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/sync_source_feedback.h)
object at startup that is responsible for sending `replSetUpdatePosition` commands.

The `SyncSourceFeedback` starts a loop. In each iteration it first waits on a condition variable
that is notified whenever the `ReplicationCoordinator` discovers that a node in the replica set has
replicated more operations and become more up-to-date. It checks that it is not in `PRIMARY` or
STARTUP state before moving on.

It then gets the node's sync source and creates a
[**`Reporter`**](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/reporter.h) that
actually sends the `replSetUpdatePosition` command to the sync source. This command keeps getting
sent every `keepAliveInterval` milliseconds (`(electionTimeout / 2)`) to maintain liveness
information about the nodes in the replica set.

`replSetUpdatePosition` commands are the primary means of maintaining liveness. Thus, if the primary
cannot communicate directly with every node, but it can communicate with every node through other
nodes, it will still stay primary.

The `replSetUpdatePosition` command contains the following information:

1. An `optimes` array containing an object for each live replica set member. This information is
   filled in by the `TopologyCoordinator` with information from its `MemberData`. Nodes that are
   believed to be down are not included. Each node contains:

   1. lastWritten opTime
   2. lastDurable OpTime
   3. lastApplied OpTime
   4. memberId
   5. `ReplicaSetConfig` version

2. `ReplSetMetadata`. Usually this only comes in responses, but here it comes in the request as
   well.

When a node receives a `replSetUpdatePosition` command, the first thing it does is have the
`ReplicationCoordinator` process the `ReplSetMetadata` as before.

For every node’s OpTime data in the `optimes` array, the receiving node updates its view of the
replicaset in the replication and topology coordinators. This updates the liveness information of
every node in the `optimes` list. If the data is about the receiving node, it ignores it. If the
receiving node is a primary and it learns that the commit point should be moved forward, it does so.

If something has changed and the receiving node itself has a sync source, it forwards its new
information to its own sync source.

The `replSetUpdatePosition` command response does not include any information unless there is an
error, such as in a `ReplSetConfig` mismatch.

#### Code References

- [OplogFetcher passes on the metadata it received from its sync source](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/oplog_fetcher.cpp#L897)
- [Node handles heartbeat response and schedules the next heartbeat after it receives heartbeat response](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L234)
- [Node responds to heartbeat request](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/repl_set_commands.cpp#L752)
- [Primary advances the replica set's commit point after receiving replSetUpdatePosition command](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L2171)
- [Secondary advances its understanding of the replica set commit point using metadata fetched from its sync source](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L5144)
- [TopologyCoordinator updates commit optime](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L2885)
- [SyncSourceFeedback triggers replSetUpdatePosition command using Reporter](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/reporter.cpp#L189)
- [Node updates replica set metadata after receiving replSetUpdatePosition command](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/repl_set_commands.cpp#L675)

## Read Concern

All reads in MongoDB are executed on snapshots of the data taken at some point in time. However, for
all read concern levels other than 'snapshot', if the storage engine yields while executing a read,
the read may continue on a newer snapshot. Thus, reads are currently not guaranteed to return all
data from one point in time. This means that some documents can be skipped if they are updated and
any updates that occurred since the read began may or may not be seen.

[Read concern](https://docs.mongodb.com/manual/reference/read-concern/) is an option sent with any
read command to specify at what consistency level the read should be satisfied. There are 5 read
concern levels:

- Local
- Majority
- Linearizable
- Snapshot
- Available

**Local** just returns whatever the most up-to-date data is on the node. On a primary, it does this
by reading from the storage engine's most recent snapshot. On a secondary, it performs a timestamped
read at the lastApplied, so that it does not see writes from the batch that is currently being
applied. For information on how local read concern works within a multi-document transaction, see
the [Read Concern Behavior Within Transactions](#read-concern-behavior-within-transactions) section.
Local read concern is the default read concern.

**Majority** does a timestamped read at the stable timestamp (also called the last committed
snapshot in the code, for legacy reasons). The data read only reflects the oplog entries that have
been replicated to a majority of nodes in the replica set. Any data seen in majority reads cannot
roll back in the future. Thus majority reads prevent **dirty reads**, though they often are
**stale reads**.

Read concern majority reads do not wait for anything to be committed; they just use different
snapshots from local reads. Read concern majority reads usually return as fast as local reads, but
sometimes will block. For example, right after startup or rollback when we do not have a committed
snapshot, majority reads will be blocked. Also, when some of the secondaries are unavailable or
lagging, majority reads could slow down or block.

For information on how majority read concern works within a multi-document transaction, see the
[Read Concern Behavior Within Transactions](#read-concern-behavior-within-transactions) section.

**Linearizable** read concern actually does block for some time. Linearizability guarantees that if
one thread does a write that is acknowledged and tells another thread about that write, then that
second thread should see the write. If you transiently have 2 primaries (one has yet to step down)
and you read the data from the old primary, the new one may have newer data and you may get a stale
read.

To prevent reading from stale primaries, reads block to ensure that the current node remains the
primary after the read is complete. Nodes just write a noop to the oplog and wait for it to be
replicated to a majority of nodes. The node reads data from the most recent snapshot, and then the
noop write occurs after the fact. Thus, since we wait for the noop write to be replicated to a
majority of nodes, linearizable reads satisfy all of the same guarantees of read concern majority,
and then some. Linearizable read concern reads are only done on the primary, and they only apply to
single document reads, since linearizability is only defined as a property on single objects.

Linearizable read concern is not allowed within a multi-document transaction.

**Snapshot** read concern is available outside of transactions for select read commands on
the primary and secondary. They are `find`, `aggregate`, and `distinct` (on unsharded
collections). For more information about snapshot reads within transactions, see the
[Read Concern Behavior Within Transactions](#read-concern-behavior-within-transactions)
section.

**Available** read concern behaves identically to local read concern in most cases. The exception is
reads for sharded collections from secondary shard nodes. Local read concern will wait to refresh
the routing table cache when the node realizes its
[metadata is stale](../s/README.md#when-the-routing-table-cache-will-refresh), which requires
contacting the shard's primary or config servers before being able to serve the read. Available read
concern does not provide consistency guarantees because it does not wait for routing table cache
refreshes. As a result, available read concern potentially serves reads faster and is more tolerant
to network partitions than any other read concern, since the node does not need to communicate with
another node in the cluster to serve the read. However, this also means that if the node's metadata
was stale, available read concern could potentially return
[orphan documents](../s/README.md#orphan-filtering) or even a stale view of a chunk that has been
moved a long time ago and modified on another shard.

Available read concern is not allowed to be used with causally consistent sessions or transactions.

**afterOpTime** is another read concern option, only used internally, only for config servers as
replica sets. **Read after optime** means that the read will block until the node has replicated
writes after a certain OpTime. This means that if read concern local is specified it will wait until
the local snapshot is beyond the specified OpTime. If read concern majority is specified it will
wait until the committed snapshot is beyond the specified OpTime.

**afterClusterTime** is a read concern option used for supporting **causal consistency**.

<!-- TODO: link to the Causal Consistency section of the Sharding Architecture Guide -->

#### Code References

- [ReadConcernArg is filled in \_extractReadConcern()](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/service_entry_point_common.cpp#L261)

## Read Preference

The [read preference](https://www.mongodb.com/docs/manual/core/read-preference/) set on a read
operation determines which nodes in a replica set are eligible to serve that operation. It allows
the user to control where and how read operations are directed within a replica set. The accepted
modes for read preference are `primary`, `primaryPreferred`, `secondary`, `secondaryPreferred`,
and `nearest`. The formal definitions and additional command format information can be found
[in the driver specification](https://github.com/mongodb/specifications/blob/master/source/server-selection/server-selection.rst#read-preference).

### Server Selection

Server selection is the process of selecting a node for a command. The client first filters servers
by specified read preference, and then if there is more than one eligible server, filters the
remaining servers based on latency. The server selection specification is fulfilled by any MongoDB
replica set client that can select from multiple servers to execute reads, such as a driver or
`mongos`. The specification determines the algorithms for filtering servers based on
`readPreference` mode, formulas for calculating roundtrip times, etc.

### Passing `$readPreference` as a parameter

A client will pass any non-primary read preference to the selected server in the form of a
`$readPreference` parameter attached to each operation. In this context, a server means either
a replica set node or `mongos`. If the read preference parameter is omitted, the server will assume
read preference `primary`. In the case of a replica set, `$readPreference` is passed to the
targeted node [to validate](https://github.com/mongodb/mongo/blob/r7.1.0/src/mongo/db/service_entry_point_common.cpp#L1642-L1658)
that the replica set state still aligns with the desired read preference.

For sharded clusters, the client skips filtering servers based on read preference and passes the
`$readPreference` directly to `mongos`. The `mongos` instance then carries out the read preference
matching on the appropriate shard and forwards the `$readPreference` to the shard server for
validation. It’s worth noting that if multiple `mongos` nodes exist in the topology, the driver
will still filter based on latency.

### Replica Set State and Read Preference

Replica set nodes receive the `$readPreference` in a command invocation for validation purposes.
This is to ensure that the given `$readPreference` matches the current state of the node, as the
replica set state may have changed in the time between the client’s server selection and the node
receiving the command. If it doesn't match, the operation will fail with one of the error codes
mentioned below. Note that there are still edge cases for `primary` or `secondary` read preferences.
With lock-free reads, a node can validate an operation's read preference in the command layer and
perform a state transition before the read succeeds, as reads are not killed on step up or step down.
This can result in a newly-stepped down secondary servicing a `primary` read preference, or a
newly-stepped up primary servicing a `secondary` read preference.

Commands can define whether or not they can run on a secondary by overriding the
[`secondaryAllowed`](https://github.com/mongodb/mongo/blob/r7.1.0/src/mongo/db/commands.h#L502-L509)
function. If a secondary node receives an operation it cannot service, it will either fail with a
`NotWritablePrimary` error if the command is designated as primary-only, or a `NotPrimaryNoSecondaryOk`
error if the command can be serviced by a secondary but the operation’s`$readPreference` specifies
primary-only. A primary node that receives a `secondary` read preference operation will service it,
although this case is rare since it requires the node to step up before it receives the operation.

# Transactions

**Multi-document transactions** were introduced in MongoDB to provide atomicity for reads and writes
to multiple documents either in the same collection or across multiple collections. Atomicity in
transactions refers to an "all-or-nothing" principle. This means that when a transaction commits,
it will not commit some of its changes while rolling back others. Likewise, when a transaction
aborts, all of its operations abort and all corresponding data changes are aborted.

## Life of a Multi-Document Transaction

All transactions are associated with a server session and at any given time,
[only one open transaction can be associated with a single session](https://github.com/mongodb/mongo/blob/r6.0.5/src/mongo/db/service_entry_point_common.cpp#L881-L902).
The state of a transaction is maintained
through the [`TransactionParticipant`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/transaction_participant.h),
which is a decoration on the session. Any thread that attempts to modify the state of the
transaction, which can include committing, aborting, or adding an operation to the transaction, must
have the correct session checked out before doing so. Only one operation can check out a session at
a time, so other operations that need to use the same session must wait for it to be checked back in.

### Starting a Transaction

Transactions are started on the server by the first operation in the transaction, indicated by a
`startTransaction: true` parameter. All operations in a transaction must include an `lsid`, which is
a unique ID for a session, a `txnNumber`, and an `autocommit:false` parameter. The `txnNumber` must
be higher than the previous `txnNumber` on this session. Otherwise, we will
[throw a `TransactionTooOld` error](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/transaction_participant.cpp#L957-L978).

When starting a new transaction, we implicitly abort the previously running transaction (if one
exists) on the session by updating our `txnNumber`. Next, we update our `txnState` to
`kInProgress`. The `txnState` maintains the state of the transaction and allows us to determine
legal state transitions. Finally, we reset the in memory state of the transaction as well as any
[corresponding transaction metrics](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/transaction_participant.cpp#L896-L902) from a previous transaction.

When a node starts a transaction, it will [acquire the global lock in intent exclusive mode](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction/transaction_participant.cpp#L1569)
(and as a result, the [RSTL](#replication-state-transition-lock) in intent exclusive as well), which it will
hold for the duration of the transaction. The only exception is when
[preparing a transaction](#preparing-a-transaction-on-the-primary), which will release the RSTL and
reacquire it when [committing](#committing-a-prepared-transaction) or
[aborting](#aborting-a-prepared-transaction) the transaction. It also
[opens a `WriteUnitOfWork`, which begins a storage engine transaction on the `RecoveryUnit`](https://github.com/mongodb/mongo/blob/411e11d88eaa52d70d02cab8e94d3a5b224900ab/src/mongo/db/transaction/transaction_participant.cpp#L1571-L1577).
The `RecoveryUnit` is responsible for making sure data is persisted and all on-disk data must be modified through this interface. The
storage transaction is updated every time an operation comes in so that we can read our own writes
within a multi-document transaction. These changes are not visible to outside operations because the
node hasn't committed the transaction (and therefore, the WUOW) yet.

### Adding Operations to a Transaction

A user can [add additional operations](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/op_observer_impl.cpp#L554) to an existing multi-document transaction by running more
commands on the same session. These operations are then stored in memory. Once a write completes on
the primary, [we update the corresponding `sessionTxnRecord`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/op_observer_impl.cpp#L1664-L1673)
in the transactions table (`config.transactions`) with information about the transaction.
This includes things like the `lsid`, the `txnNumber` currently associated with the session, and the `txnState`.

This table was introduced for retryable writes and is used to keep track of retryable write and
transaction progress on a session. When checking out a session, this table can be used to restore
the transaction's state. See the
[Recovering Prepared Transactions](#recovering-prepared-transactions) section for information on how
the transactions table is used during transaction recovery.

### Committing a Single Replica Set Transaction

If we decide to commit this transaction, [we retrieve those operations, group them into an `applyOps`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/op_observer_impl.cpp#L1981-L1983)
command and write down an `applyOps` oplog entry. Since an `applyOps` oplog entry can only be up to
16MB, transactions larger than this require multiple `applyOps` oplog entries upon committing.

If we are committing a [read-only transaction](#read-concern-behavior-within-transactions), meaning that we did not modify any data, it must wait
for any data it reads to be majority committed regardless of the `readConcern` level.

Once we log the transaction oplog entries, [we must commit the storage-transaction](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/transaction_participant.cpp#L1850-L1852)
on the `OperationContext`. This involves calling commit() on the WUOW. Once commit() is called on the WUOW
associated with a transaction, all writes that occurred during its lifetime will commit in the
storage engine.

Finally, we update the transactions table, [update our local `txnState` to `kCommitted`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/transaction_participant.cpp#L2012), log any
transactions metrics, and clear our txnResources.

### Aborting a Single Replica Set Transaction

The process for aborting a multi-document transaction is simpler than committing. We [abort](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/transaction_participant.cpp#L2072) the
storage transaction and change our local `txnState` to `kAbortedWithoutPrepare`. We then log any
transactions metrics and reset the in memory state of the `TransactionParticipant`. None of the
transaction operations are visible at this point, so we don't need to write an abort oplog entry
or update the transactions table.

Note that transactions can abort for reasons outside of the `abortTransaction` command. For example,
we abort non-prepared transactions that encounter write conflicts or state transitions.

## Cross-Shard Transactions and the Prepared State

In 4.2, we added support for **cross-shard transactions**, or transactions that involve data from
multiple shards in a cluster. We needed to add a **Two Phase Commit Protocol** to uphold the
atomicity of a transaction that involves multiple shards. One important part of the Two Phase Commit
Protocol is making sure that all shards participating in the transaction are in the
**prepared state**, or guaranteed to be able to commit, before actually committing the transaction.
This will allow us to avoid a situation where the transaction only commits on some of the shards and
aborts on others. Once a node puts a transaction in the prepared state, it _must_ be able to commit
the transaction if we decide to commit the overall cross-shard transaction.

Another key piece of the Two Phase Commit Protocol is the [**`TransactionCoordinator`**](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/s/transaction_coordinator.h#L70), which is
the first shard to receive an operation for a particular transaction. The `TransactionCoordinator`
will coordinate between all participating shards to ultimately commit or abort the transaction.

When the `TransactionCoordinator` is [told to commit a transaction](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/transaction_coordinator_service.cpp#L175-L176), it must first make sure that all
participating shards successfully prepare the transaction before telling them to commit the
transaction. As a result, the coordinator will [issue the `prepareTransaction` command](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/s/transaction_coordinator.cpp#L286-L317), an internal
command, on each shard participating in the transaction.

Each participating shard must majority commit the `prepareTransaction` command (thus making sure
that the prepare operation cannot be rolled back) before the `TransactionCoordinator` will [send out
the `commitTransaction` command](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/s/transaction_coordinator.cpp#L402-L408). This will help ensure that once a node prepares a transaction, it
will remain in the prepared state until the transaction is committed or aborted by the
`TransactionCoordinator`. If one of the shards fails to prepare the transaction, the
`TransactionCoordinator` will [tell all participating shards to abort the transaction](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/s/transaction_coordinator.cpp#L405-L410) via the
`abortTransaction` command regardless of whether they have prepared it or not.

The durability of the prepared state is managed by the replication system, while the Two Phase
Commit Protocol is managed by the sharding system.

## Lifetime of a Prepared Transaction

Until a `prepareTransaction` command is run for a particular transaction, it follows the same path
as a single replica set transaction. But once a transaction is in the prepared state, new operations
cannot be added to it. The only way for a transaction to exit the prepared state is to either
receive a `commitTransaction` or `abortTransaction` command. This means that prepared transactions
must [survive state transitions and failovers](#state-transitions-and-failovers-with-transactions).
Additionally, there are many situations that need to be prevented to preserve prepared transactions.
For example, they cannot be killed or time out (nor can their sessions), manual updates to the
transactions table are forbidden for transactions in the prepared state, and the prepare transaction
oplog entry(s) cannot fall off the back of the oplog.

### Preparing a Transaction on the Primary

When a primary receives a `prepareTransaction` command, it will [transition the associated
transaction's `txnState` to `kPrepared`](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/transaction/transaction_participant.cpp#L1844). Next it will [reserve an **oplog slot**](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1739-L1754) (which is a unique
`OpTime`) for the `prepareTransaction` oplog entry. The `prepareTransaction` oplog entry will
contain all the operations from the transaction, which means that if the transaction is larger than
16MB (and thus requires multiple oplog entries), the node will reserve multiple oplog slots. The
`OpTime` for the `prepareTransaction` oplog entry will be used for the
[**`prepareTimestamp`**](#replication-timestamp-glossary).

The node will then [set the `prepareTimestamp`](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1779) on the `RecoveryUnit` and mark the storage engine's
transaction as prepared so that the storage engine can
[block conflicting reads and writes](#prepare-conflicts) until the transaction is committed or
aborted.

Next, the node will [create the `prepareTransaction` oplog entry](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_operations.cpp#L230) and [write it to the oplog](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1785-L1798). This will
involve taking all the operations from the transaction and storing them as an `applyOps` oplog
entry (or multiple `applyOps` entries for larger transactions). The node will also [make a couple updates to the transactions table](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/op_observer/op_observer_impl.cpp#L1495-L1505).
It will update the starting `OpTime` of the transaction, which
will either be the `OpTime` of the prepare oplog entry or, in the case of larger transactions, the
`OpTime` of the first oplog entry of the transaction. It will also update that the state of the
transaction is `kPrepared`. This information will be useful if the node ever needs to recover the
prepared transaction in the event of failover.

If any of the above steps fails when trying to prepare a transaction, then the node will abort the
transaction. If that happens, the node will respond back to the `TransactionCoordinator` that the
transaction failed to prepare. This will cause the `TransactionCoordinator` to tell all other
participating shards to abort the transaction, thus preserving the atomicity of the transaction. If
this happens, it is safe to retry the entire transaction.

Finally, the node will record metrics, [release](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1826-L1829) the [RSTL](#replication-state-transition-lock) (while
still holding the global lock) to allow prepared transactions to survive state transitions, and
respond with the `prepareTimestamp` to the `TransactionCoordinator`.

### Prepare Conflicts

A **prepare conflict** is generated when an operation attempts to read a document that was updated
as a part of an active prepared transaction. Since the transaction is still in the prepared state,
it's not yet known whether it will commit or abort, so updates made by a prepared transaction can't
be made visible outside the transaction until it completes.

Based on the read concern, reads will do different things in this case. A read with read concern
local, available or majority (without causal consistency) will not cause a prepare conflict to be
generated by the storage engine, but instead will return the state of the data before the prepared
update. Reads using snapshot, linearizable, or afterClusterTime read concerns, will block and wait
until the transaction is committed or aborted to serve the read.

If a write attempts to modify a document that was also modified by a prepared transaction, it will
block and wait for the transaction to be committed or aborted before proceeding.

This is handled in [wiredTigerPrepareConflictRetry](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h#L74).

### Committing a Prepared Transaction

Committing a prepared transaction is very similar to
[committing a single replica set transaction](#committing-a-single-replica-set-transaction). One of
the main differences is that the commit oplog entry will not have any of the operations from the
transaction in it, because those were already included in the prepare oplog entry(s).

For a cross-shard transaction, the `TransactionCoordinator` will issue the `commitTransaction`
command to all participating shards when each shard has majority committed the `prepareTransaction`
command. The `commitTransaction` command must be run with a specified
[`commitTimestamp`](#replication-timestamp-glossary) so that all participating shards can commit the
transaction at the same timestamp. This will be the timestamp at which the effects of the
transaction are visible.

When a node receives the `commitTransaction` command and the transaction is in the prepared state,
it will first [re-acquire](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1962) the [RSTL](#replication-state-transition-lock) to prevent any state
transitions from happening while the commit is in progress. It will then [reserve an oplog slot](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2021-L2030),
[commit the storage transaction at the `commitTimestamp`](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2057-L2059),
[write the `commitTransaction` oplog entry](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2065-L2069)
into the oplog, [update the transactions table](https://github.com/mongodb/mongo/blob/master/src/mongo/db/op_observer/op_observer_impl.cpp#L201), transition the `txnState` to `kCommitted`, record
metrics, and [clean up the transaction resources](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2073-L2075).

### Aborting a Prepared Transaction

Aborting a prepared transaction is very similar to
[aborting a non-prepared transaction](#aborting-a-single-replica-set-transaction). The only
difference is that before aborting a prepared transaction, the node must [re-acquire](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2251) the
[RSTL](#replication-state-transition-lock) to prevent any state transitions from happening while
the abort is in progress. Non-prepared transactions don't have to do this because the node will
still have the RSTL at this point. We then [reserve an oplog slot](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2290-L2293),
[abort the storage transaction](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L2303),
and [write the abortTransaction oplog entry](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/transaction/transaction_participant.cpp#L2444).

## State Transitions and Failovers with Transactions

### State Transitions and Failovers with Single Replica Set Transactions

The durability of a single replica set transaction is not guaranteed until the `commitTransaction`
command is majority committed. This means that in-progress transactions are not recovered during
failover or preserved during state transitions.

Unprepared transactions that are in-progress will be aborted during stepdown. During a
[stepdown](#step-down), transactions will still be holding the
[RSTL](#replication-state-transition-lock), which will conflict with the stepdown. As a result,
after enqueueing the RSTL during stepdown, the node will abort all unprepared transactions until it
can acquire the RSTL.

Transactions that are in-progress but not in the prepared state will be aborted during
[step up](#step-up). Being in progress during step up means that the transaction requires multiple
oplog entries and that the node has not received all the oplog entries it needs to prepare or commit
the transaction. As a result, the node will abort all such transactions before it steps up.

If a node goes through a shut down, it will not recover any unprepared transactions during
[startup recovery](#startup-recovery).

### Stepdown with a Prepared Transaction

Unlike unprepared transactions, which get aborted during a stepdown, prepared transactions need to
survive stepdown because shards are relying on the `prepareTransaction` command being (and
remaining) majority committed. As a result, after preparing a transaction, the node will [release](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1826-L1829) the
[RSTL](#replication-state-transition-lock) so that it does not end up conflicting with state
transitions. When [stepdown](#step-down) is aborting transactions before acquiring the RSTL, it will
only abort unprepared transactions. Once stepdown finishes, the node will [yield locks from all prepared transactions](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L3890)
since secondaries don't hold locks for their transactions.

### Step Up with a Prepared Transaction

If a secondary has a prepared transaction when it [steps up](#step-up), it will have to [re-acquire all the locks](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.cpp#L106)
for the prepared transaction (other than the RSTL), since the primary relies on
holding these locks to prevent conflicting operations.

### Recovering Prepared Transactions

The prepare state _must_ endure any state transition or failover, so they must be recovered and
reconstructed in all situations. If the in-memory state of a prepared transaction is lost, it can be
reconstructed using the information in the prepare oplog entry(s).

[Startup recovery](#startup-recovery), [rollback](#rollback), and [initial sync](#initial-sync) all
use the same algorithm to reconstruct prepared transactions. In all situations, the node will go
through a period of applying oplog entries to get the data caught up with the rest of the replica
set.

As the node applies oplog entries, it will update the transaction table every time it encounters a
`prepareTransaction` oplog entry to save that the state of the transaction is prepared. Instead of
actually applying the oplog entry and preparing the transaction, the node will wait until oplog
application [has completed](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/replication_recovery.cpp#L472) to reconstruct the transaction. If the node encounters a
`commitTransaction` oplog entry, it will immediately commit the transaction. If the transaction it's
about to commit was prepared, the node will find the `prepareTransaction` oplog entry(s) using the
[`TransactionHistoryIterator`](https://github.com/mongodb/mongo/blob/v6.1/src/mongo/db/transaction/transaction_history_iterator.h)
to get the operations for the transaction, prepare it and then immediately commit it.

When oplog application for recovery or initial sync completes, the node will iterate
over all entries in the transactions table to see which transactions are still in the prepared
state. At that point, the node will find the `prepareTransaction` oplog entry(s) associated with the
transaction using the `TransactionHistoryIterator`. It will check out the session associated with
the transaction, apply all the operations from the oplog entry(s) and prepare the transaction.

#### Code references

- Function to [abort unprepared transactions during stepup or stepdown](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_step_up_step_down.cpp#L164).
- Where we [yield locks for transactions](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1282-L1287).
- Where we [restore locks for transactions](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1343-L1348).
- Function to [reconstruct prepared transactions from oplog entries](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/transaction_oplog_application.cpp#L804).
- Where we [skip over prepareTransaction oplog entries](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/transaction_oplog_application.cpp#L737-L752) during recovery oplog application.

## Read Concern Behavior Within Transactions

The read concern for all operations within a transaction should be specified when starting the
transaction. If no read concern was specified, the default read concern is local.

Reads within a transaction behave differently from reads outside of a transaction because of
**speculative** behavior. This means a transaction speculatively executes without ensuring that
the data read won't be rolled back until it commits. No matter the read concern, when a node goes to
commit a transaction, it waits for the data that it read to be majority committed _as long as the
transaction was run with write concern majority_. Because of speculative behavior, this means that
the transaction can only provide the guarantees of majority read concern, that data that it read
won't roll back, if it is run with write concern majority.

If the transaction did a write, then waiting for write concern is enough to ensure that all data
read will have since become majority committed. However, if the transaction was read-only, the node
will do a noop write and wait for that to be majority committed to provide the same guarantees.

### Local and Majority Read Concerns

There is currently no functional difference between a transaction with local and majority read
concern. The node will do untimestamped reads in either case. When the transaction is started, it
will choose the most recent snapshot to read from, so that it can read the freshest data.

The node does untimestamped reads because reading at the
[`all_durable`](#replication-timestamp-glossary) timestamp would mean that the node was reading
potentially stale data. This would also allow for more write conflicts that abort the transaction,
since there would be a larger window of time between when the read happens and when the transaction
commits for an outside write to conflict.

In theory, transactions with local read concern should not have to perform a noop write and wait for
it to be majority committed when the transaction commits. However, we intend to make "majority" the
default read concern level for transactions, in order to be consistent with the observation that any
MongoDB command that can write has speculative majority behavior (e.g. findAndModify). Until we
change transactions to have majority as their default read concern, it's important that local and
majority behave the same.

### Snapshot Read Concern

Snapshot read concern will choose a snapshot from which the transaction will read. If it is
specified with an `atClusterTime` argument, then that will be used as the transaction's read
timestamp. If `atClusterTime` is not specified, then the read timestamp of the transaction will be
the [`all_durable`](#replication-timestamp-glossary) timestamp when the transaction is started,
which ensures a snapshot with no oplog holes.

#### Code references

- [Noop write for read-only transactions](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1940-L1944).
- Function to [set a read snapshot for transactions](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1170).

## Transaction Oplog Application

Secondaries begin replicating transaction oplog entries once the primary has either prepared or
committed the transaction. They use the `OplogApplier` to apply these entries, which then uses the
writer thread pool to schedule operations to apply. See the
[oplog entry application](#oplog-entry-application) section for more details on how secondary oplog
application works.

Before secondaries process and apply transaction oplog entries, they will [track operations](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/oplog_applier_impl.cpp#L968) that
require changes to `config.transactions`. This results in an [update to the transactions table entry](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/session_update_tracker.cpp#L336)
(`sessionTxnRecord`) that corresponds to the oplog entry operation. For example,
`prepareTransaction`, `commitTransaction`, and `abortTransaction` will all update the `txnState`
accordingly.

### Unprepared Transactions Oplog Application

Unprepared transactions are comprised of `applyOps` oplog entries. When they are smaller than 16MB,
they will write a single `applyOps` oplog entry for the whole transaction upon commit. Since
secondaries do not need to wait for any additional entries, they can apply the entry for a small
unprepared transaction immediately.

When transactions are larger than 16MB, they use the `prevOpTime` field, which is the opTime of the
previous `applyOps` oplog entry, to link multiple `applyOps` oplog entries together. The
`partialTxn: true` field is used here to indicate that the transaction is incomplete and cannot be
applied immediately. Since the `partialTxn` field does not apply to all oplog entries, it is added
as a subfield of the 'o' field. A secondary must wait until it receives the final `applyOps` oplog
entry of a large unprepared transaction, which will have a non-empty `prevOpTime` field and no
`partialTxn` field, before applying entries. This ensures we have all the entries associated with
the transaction before applying them, allowing us to avoid failover scenarios where only part of a
large transaction is replicated.

When we see an `applyOps` oplog entry that is a part of an unprepared transaction, we will unpack
the CRUD operations and apply them in parallel by using the writer thread pool. For larger
transactions, once the secondary has received all the oplog entries, it will traverse the oplog
chain to get all the operations from the transaction and do the same thing.

Note that checking out the session is not necessary for unprepared transactions since they are
just a series of CRUD operations for secondary oplog application. The atomicity of data on disk is
guaranteed by recovery. The atomicity of visible data in memory is guaranteed by how we advance the
[`lastApplied`](#replication-timestamp-glossary) on secondaries.

### Prepared Transactions Oplog Application

Prepared transactions also write down `applyOps` oplog entries that contain all the operations for
the transaction, but do so when they are prepared. Prepared transactions smaller than 16MB only
write one of these entries, while those larger than 16MB write multiple.

We use a `prepare: true` field to indicate that an `applyOps` entry is for a prepared transaction.
For large prepared transactions, this field will be present in the last `applyOps` entry of the
oplog chain, indicating that the secondary must prepare the transaction. The timestamp of the
prepare oplog entry is referred to as the [`prepareTimestamp`](#replication-timestamp-glossary).

`prepareTransaction` oplog entries are applied in their own batches in a single WUOW. When
applying prepared operations, which are unpacked from the prepared `applyOps` entry, the applier
thread must first check out the appropriate session and **unstash** the transaction resources
(`txnResources`). `txnResources` refers to the lock state and storage state of a transaction. When
we "unstash" these resources, we transfer the management of them to the `OperationContext`. The
applier thread will then add the prepare operations to the storage transaction and finally yield
the locks used for transactions. This means that prepared transactions will only **stash** (which
transfers the management of `txnResources` back to the session) the recovery unit. Stashing the
locks would make secondary oplog application conflict with prepared transactions. These locks are
restored the next time we **unstash** `txnResources`, which would be for `commitTransaction` or
`abortTransaction`.

Prepared transactions write down separate `commitTransaction` and `abortTransaction` oplog entries.
`commitTransaction` oplog entries do not need to store the operations from the transaction since
we have already recorded them through the prepare oplog entries. These entries are also applied
in their own batches and follow the same procedure of checking out the appropriate session,
unstashing transaction resources, and either committing or aborting the storage transaction.

Note that secondaries can apply prepare oplog entries immediately but
[recovering](#recovering-prepared-transactions) nodes must wait until they finish the process or
see a commit oplog entry.

Another major difference between secondary nodes and recovering nodes is that recovering nodes
process prepare oplog entries one at a time and operations in a prepare oplog entry are applied in
serial, while secondary nodes batch process prepare oplog entries and use multiple threads to
parallelize the application of operations in each prepare oplog entry.

In order to parallelize the application, a `prepareTransaction` oplog entry can be
[applied in the same batch](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/repl/oplog_batcher.cpp#L243-L248)
as other CRUD or `prepareTransaction` oplog entries, and operations in each `prepareTransaction`
oplog entry are [split among the writer threads](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/repl/oplog_applier_utils.cpp#L256)
in the same way as [applying a normal oplog entry](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/repl/README.md#oplog-entry-application).
This splitting mechanism ensures operations on one document are applied by only one thread,
together with the primary ensuring no prepare conflicts between concurrent prepared transactions
and concurrent CRUD operations, we make it possible to fully parallelize the application of
`prepareTransaction` oplog entries with other CRUD or `prepareTransaction` oplog entries. Each
writer thread that gets assigned a subset of the transaction operations basically starts a split
prepared transaction with a new session and apply it using the steps described above. This means
that one `prepareTransaction` oplog entry might create multiple smaller prepared transactions.
All the sessions of the original prepared transactions and their split sessions, as well as the IDs
of those writer threads are tracked in the [SplitPrepareSessionManager](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/repl/split_prepare_session_manager.h)
class.

A `commitTransaction` or `abortTransaction` oplog entry on steady state secondary nodes may refer
to a non-split prepared transaction (e.g. prepared while being primary or during recovery) or a
split prepared transaction. The former case is handled in the same way as on recovering nodes.
For the latter case, we first query the `SplitPrepareSessionManager` to get the sessions and
thread IDs that have been used when splitting and applying the corresponding `prepareTransaction`
oplog entry, and then [split the commitTransaction or abortTransaction oplog entry](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/repl/oplog_applier_utils.cpp#L340-L348)
to the same threads to make sure that each split of the original prepared transaction is correctly
committed or aborted.

Note it is possible for a secondary node to step up after applying a split prepared transaction,
thus when a primary node receives a `commitTransaction` command, it needs to
[additionally commit all the splits](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/transaction/transaction_participant.cpp#L1951-L1958)
of the original prepared transaction if they exist. Another caveat due to step-up is that we need
to prepare and commit/abort the original transaction (a.k.a. top-level transaction) in addition to
its split transactions, so that on step-up the in-memory transaction states of the original
transaction's session is correctly set, otherwise the session cannot be used to run new transaction
commands. However we do not need to apply any operations in the original transaction (treated like
an [empty transaction](https://github.com/mongodb/mongo/blob/07e1e93c566243983b45385f5c85bc7df0026f39/src/mongo/db/repl/transaction_oplog_application.cpp#L720-L731))
since the operations should be applied by its split transactions.

#### Code references

- [Filling writer vectors for unprepared transactions on terminal applyOps.](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/oplog_applier_impl.cpp#L1018-L1033)
- [Applying writes in parallel](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/repl/oplog_applier_impl.cpp#L809-L832) via the writer thread pool.
- Function to [unstash transaction resources](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1462) from the RecoveryUnit to the OperationContext.
- Function to [stash transaction resources](https://github.com/mongodb/mongo/blob/be38579dc72a40988cada1f43ab6695dcff8cc36/src/mongo/db/transaction/transaction_participant.cpp#L1427) from the OperationContext to the RecoveryUnit.

## Transaction Errors

### PreparedTransactionInProgress Errors

Starting a new transaction on a session with an already existing in-progress transaction can cause
the existing transaction to be **implicitly aborted**. Implicitly aborting a transaction happens if
the transaction is aborted without an explicit `abortTransaction` command. However, prepared
transactions cannot be implicitly aborted, since they can only complete after a `commitTransaction`
or `abortTransaction` command from the `TransactionCoordinator`. As a result, any attempt to start a
new transaction on a session that already has a prepared transaction on it will fail with a
`PreparedTransactionInProgress` error.

Additionally, the only operations that can be run on a prepared transaction are
`prepareTransaction`, `abortTransaction`, and `commitTransaction`. If any other command is run on
the transaction, the command will fail with a `PreparedTransactionInProgress` error.

Commands run as a part of a transaction that fail with this error code will always have the
[`TransientTransactionError`](#transienttransactionerror-label) label attached to its response.

### NoSuchTransaction Errors

`NoSuchTransaction` errors are generated for commands that attempt to continue, prepare, commit, or
abort a transaction that isn't in progress. Two common reasons why this error is generated are
because the transaction has since started a new transaction or the transaction has already been
aborted.

All commands made in a transaction other than `commitTransaction` that fail with this error code
will always have the [`TransientTransactionError`](#transienttransactionerror-label) label attached
to its response. If the `commitTransaction` command failed with a `NoSuchTransaction` error without
a write concern error, then it will have the `TransientTransactionError` label attached to its
response. If it failed with a write concern error, then it wouldn't be safe to retry the entire
transaction since it could have committed on one of the nodes.

### TransactionTooOld Errors

If an attempt is made to start a transaction that is older than the current active or the last
committed transaction, then that operation will fail with `TransactionTooOld`.

### TransientTransactionError Label

A transaction could fail with one of the errors above or a different one. There are some errors that
will cause a transaction to abort with no persistent side effects. In these cases, the server will
attach the `TransientTransactionError` label to the response (which will still contain the orignal
error code), so that the caller knows that they can safely retry the entire transaction.

# Concurrency Control

## Replication State Transition Lock

When a node goes through state transitions, it needs something to manage the concurrency of that
state transition with other ongoing operations. For example, a node that is stepping down used to be
able to accept writes, but shouldn't be able to do so until it becomes primary again. As a result,
there is the **Replication State Transition Lock** (or RSTL), a global resource that manages the
concurrency of state transitions.

It is acquired in exclusive mode for the following replication state transitions: `PRIMARY` to
`SECONDARY` (step down), `SECONDARY` to `PRIMARY` (step up), `SECONDARY` to `ROLLBACK` (rollback),
`ROLLBACK` to `SECONDARY`, and `SECONDARY` to `RECOVERING`. Operations can hold it when they need to
ensure that the node won't go through any of the above state transitions. Some examples of
operations that do this are [preparing](#preparing-a-transaction-on-the-primary) a transaction,
[committing](#committing-a-prepared-transaction) or [aborting](#aborting-a-prepared-transaction) a
prepared transaction, and checking/setting if the node can accept writes or serve reads.

## Global Lock Acquisition Ordering

Both the MultiDocumentTransactionsBarrier lock and RSTL are global resources that are acquired
before the global lock is acquired.

First, the MultiDocumentTransactionsBarrier lock is only acquired when
the request is part of a multi-document transaction, or when the global lock is requested in
[shared](https://www.mongodb.com/docs/manual/reference/glossary/#std-term-read-lock) or
[exclusive](https://www.mongodb.com/docs/manual/reference/glossary/#std-term-write-lock) mode;
the lock is acquired in the same mode as the global lock request.
Next, it must acquire the RSTL in [intent exclusive](https://docs.mongodb.com/manual/reference/glossary/#term-intent-lock)
mode. Only then can it acquire the global lock in its desired mode.

# Non-transactional batched operations.

## Vectored inserts

For vectored inserts, where one user command inserts multiple documents, but not within a
multi-document transaction, we use a special mechanism to reduce replication overhead. The
documents are broken up into batches with a maximum of `internalInsertMaxBatchSize` documents, or
`insertVectorMaxBytes` bytes, whichever results in a smaller batch.

When we open the `WriteUnitOfWork` for these batches, we specify a
`WriteUnitOfWork::OplogEntryGroupType` of `kGroupForPossiblyRetryableOperations`. This will set the
`writesAreBatched` flag on the `BatchedWriteContext` decoration on the OperationContext. When this
flag is set, the `OpObserverImpl` will collect writes in a `BatchedOperations` (an alias for
`TransactionOperations`) structure on the `BatchedWriteContext` rather than write oplog entries for
them.

When the `WriteUnitOfWork` commits, the OpObserverImpl will write these oplog entries in an
`applyOps` entry. If this insert is not within a retryable session, this applyOps entry will lack
the `lsid`, `txnNumber`, and `prevOpTime` fields. If this insert is within a retryable session, it
will contain all those fields and also a `multiOpType: 1` field which distinguishes vectored inserts
from transactions. All `applyOps` entries generated from batches within a single retryable vectored
insert will have the same `lsid` and `txnNumber`, and will be linked to the previous entry using the
`prevOpTime` field, which will be a null optime for the first `applyOps`.

It is expected that users of the `kGroupForPossiblyRetryableOperations` parameter will ensure that
no more than `BSONMaxUserSize` bytes of user data are inserted within one `WriteUnitOfWork`. If
this is exceeded, multiple `applyOps` entries will be generated, with sequential optimes; if they
are within a retryable write they will be linked together.

When applied on a secondary, each `applyOps` in a batched operation will be applied separately;
unlike transactions, there is no requirement that all writes within a single vectored insert are
applied in a single batch.

(n.b. while this mechanism is intended to be able to handle multiple types of operations, only
vectored inserts are currently implemented and features necessary for other operations, such as
pre- and post- image recording are not currently implemented)

# Elections

## Step Up

There are a number of ways that a node will run for election:

- If it hasn't seen a primary within the election timeout (which defaults to 10 seconds).
- If it realizes that it has higher priority than the primary, it will wait and run for
  election (also known as a **priority takeover**). The amount of time the node waits before calling
  an election is directly related to its priority in comparison to the priority of rest of the set
  (so higher priority nodes will call for a priority takeover faster than lower priority nodes).
  Priority takeovers allow users to specify a node that they would prefer be the primary.
- Newly elected primaries attempt to catchup to the latest applied OpTime in the replica
  set. Until this process (called primary catchup) completes, the new primary will not accept
  writes. If a secondary realizes that it is more up-to-date than the primary and the primary takes
  longer than `catchUpTakeoverDelayMillis` (default 30 seconds), it will run for election. This
  behvarior is known as a **catchup takeover**. If primary catchup is taking too long, catchup
  takeover can help allow the replica set to accept writes sooner, since a more up-to-date node will
  not spend as much time (or any time) in catchup. See the [Transitioning to `PRIMARY` section](https://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/README.md#transitioning-to-primary) section for
  further details on primary catchup.
- The `replSetStepUp` command can be run on an eligible node to cause it to run for election
  immediately. We don't expect users to call this command, but it is run internally for election
  handoff and testing.
- When a node is stepped down via the `replSetStepDown` command, if the `enableElectionHandoff`
  parameter is set to true (the default), it will choose an eligible secondary to run the
  `replSetStepUp` command on a best-effort basis. This behavior is called **election handoff**. This
  will mean that the replica set can shorten failover time, since it skips waiting for the election
  timeout. If `replSetStepDown` was called with `force: true` or the node was stepped down while
  `enableElectionHandoff` is false, then nodes in the replica set will wait until the election
  timeout triggers to run for election.

### Code references

- [election timeout](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L495) ([defaults](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/repl_set_config.idl#L109))
- [priority takeover](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L504)
- [priority takeover: priority check](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L1568-L1578)
- [priority takeover: wait time calculation](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/repl_set_config.cpp#L705-L709)
- [newly elected primary catchup](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_catchup.cpp#L60)
- [primary catchup completion](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_catchup.cpp#L146-L158)
- [primary start accepting writes](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L1552)
- [catchup takeover](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L519)
- [catchup takeover: takeover check](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L519)
- [election handoff](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L2838)
- [election handoff: skip wait](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_step_up_step_down.cpp#L462-L467)

### Candidate Perspective

A candidate node first runs a dry-run election. In a **dry-run election**, a node starts a
[`VoteRequester`](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/vote_requester.h),
which uses a
[`ScatterGatherRunner`](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/scatter_gather_runner.h)
to send a [`replSetRequestVotes` command](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/repl_set_request_votes.cpp#L47) to every node asking if that node would vote for it. The
candidate node does not increase its term during a dry-run because if a primary ever sees a higher
term than its own, it steps down. By first conducting a dry-run election, we make it unlikely that
nodes will increase their own term when they would not win and prevent needless primary stepdowns.
If the node fails the dry-run election, it just continues replicating as normal. If the node wins
the dry-run election, it begins a real election.

If the candidate was stepped up as a result of an election handoff, it will skip the dry-run and
immediately call for a real election.

In the real election, the node first increments its term and votes for itself. It then follows the
same process as the dry-run to start a `VoteRequester` to send a `replSetRequestVotes` command to
every single node. Each node then decides if it should vote "aye" or "nay" and responds to the
candidate with their vote. The candidate node must be at least as up to date as a majority of voting
members in order to get elected.

If the candidate received votes from a majority of nodes, including itself, the candidate wins the
election.

#### Code references

- [dry-run election](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_elect_v1.cpp#L233)
- [skipping dry-run](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_elect_v1.cpp#L220)
- [real election](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_elect_v1.cpp#L303)
- [candidate process vote response](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/vote_requester.cpp#L114)
- [candidate checks election result](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_elect_v1.cpp#L444)

### Voter Perspective

When a node receives a `replSetRequestVotes` command, it first checks if the term is up to date and
updates its own term accordingly. The `ReplicationCoordinator` then asks the `TopologyCoordinator`
if it should grant a vote. The vote is rejected if:

1. It's from an older term.
2. The configs do not match (see more detail in [Config Ordering and Elections](#config-ordering-and-elections)).
3. The replica set name does not match.
4. The lastWritten OpTime that comes in the vote request is older than the voter's lastWritten
   OpTime.
5. If it's not a dry-run election and the voter has already voted in this term.
6. If the voter is an arbiter and it can see a healthy primary of greater or equal priority. This is
   to prevent primary flapping when there are two nodes that can't talk to each other and an arbiter
   that can talk to both.

Whenever a node votes for itself, or another node, it records that "LastVote" information durably to
the `local.replset.election` collection. This information is read into memory at startup and used in
future elections. This ensures that even if a node restarts, it does not vote for two nodes in the
same term.

#### Code references

- [node processing vote request](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L3429)
- [recording LastVote durably](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L5241)

### Transitioning to `PRIMARY`

Now that the candidate has won, it must become `PRIMARY`. First it clears its sync source and
notifies all nodes that it won the election via a round of heartbeats. Then the node checks if it
needs to catch up from the former primary. Since the node can be elected without the former
primary's vote, the primary-elect will attempt to replicate any remaining oplog entries it has not
yet replicated from any viable sync source. While these are guaranteed to not be committed, it is
still good to minimize rollback when possible.

The primary-elect uses the responses from the recent round of heartbeats to see the latest applied
OpTime of every other node. If the primary-elect’s lastApplied OpTime is less than the newest last
applied OpTime it sees, it will set that as its target OpTime to catch up to. At the beginning of
catchup, the primary-elect will schedule a timer for the catchup-timeout. If that timeout expires or
if the node reaches the target OpTime, then the node ends the catch-up phase. The node then clears
its sync source and stops the `OplogFetcher`.

We will ignore whether or not **chaining** is enabled for primary catchup so that the primary-elect
can find a sync source. And one thing to note is that the primary-elect will not necessarily sync
from the most up-to-date node, but its sync source will sync from a more up-to-date node. This will
mean that the primary-elect will still be able to catchup to its target OpTime. Since catchup is
best-effort, it could time out before the node has applied operations through the target OpTime.
Even if this happens, the primary-elect will not step down.

At this point, whether catchup was successful or not, the node goes into "drain mode". This is when
the node has already logged "transition to `PRIMARY`", but has not yet applied all of the oplog
entries in its oplog buffer. `replSetGetStatus` will now say the node is in `PRIMARY` state. The
applier keeps running, and when it completely drains the buffer, it signals to the
`ReplicationCoordinator` to finish the step up process. The node marks that it can begin to accept
writes. According to the Raft Protocol, we cannot update the commit point to reflect oplog entries
from previous terms until the commit point is updated to reflect an oplog entry in the current term.
The node writes a "new primary" noop oplog entry so that it can commit older writes as soon as
possible. Once the commit point is updated to reflect the "new primary" oplog entry, older writes
will automatically be part of the commit point by nature of happening before the term change.
Finally, the node drops all temporary collections, restores all locks for
[prepared transactions](#step-up-with-a-prepared-transaction), aborts all
[in progress transactions](#state-transitions-and-failovers-with-single-replica-set-transactions),
and logs “transition to primary complete”. At this point, new writes will be accepted by the
primary.

#### Code references

- [clearing the sync source, notify nodes of election, prepare catch up](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_catchup.cpp#L384-L394)
- [catchup to latest optime known via heartbeats](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_catchup.cpp#L147)
- [catchup-timeout](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_catchup.cpp#L92)
- [always allow chaining for catchup](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L4797)
- [enter drain mode after catchup attempt](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_catchup.cpp#L130)
- [exit drain mode](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L1411)
- [term bump](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L1491)
- [drop temporary collections](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L689)

## Step Down

### Conditional

The `replSetStepDown` command is one way that a node relinquishes its position as primary. Stepdown via the
`replSetStepDown` command is called "conditional" because it may or may not succeed. Success in this case
depends on the params passed to the command as well as the state of nodes of the replica set.

- If the `force` option is set to `true`:
  - In this case the primary node will wait for `secondaryCatchUpPeriodSecs`, a `replSetStepDown` parameter,
    before stepping down regardless of whether the other nodes have caught up or are electable.
- If the `force` option is omitted or set to `false`, the following conditions must be met for the command to
  succeed:
  - The [`lastApplied`](#replication-timestamp-glossary) OpTime of the primary must be replicated to a majority
    of the nodes
  - At least one of the up-to-date secondaries must also be electable

When a `replSetStepDown` command comes in, the node begins to check if it can step down. First, the
node attempts to acquire the [RSTL](#replication-state-transition-lock). In order to do so, it must
kill all conflicting user/system operations and abort all unprepared transactions.

Now, the node loops trying to step down. If force is `false`, it repeatedly checks if a majority of
nodes have reached the `lastApplied` optime, meaning that they are caught up. It must also check
that at least one of those nodes is electable. If force is `true`, it does not wait for these
conditions and steps down immediately after it reaches the `secondaryCatchUpPeriodSecs` deadline.

Upon a successful stepdown, it yields locks held by
[prepared transactions](#stepdown-with-a-prepared-transaction) because we are now a secondary.
Finally, we log stepdown metrics and update our member state to `SECONDARY`.

#### Code references

- [User-facing documentation](https://www.mongodb.com/docs/manual/reference/command/replSetStepDown/#command-fields).
- [Replication coordinator stepDown method](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_step_up_step_down.cpp#L255)
- [ReplSetStepDown command class](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/repl_set_commands.cpp#L527)
- [The node loops trying to step down](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_step_up_step_down.cpp#L377)
- [A majority of nodes need to have reached the lastApplied optime](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L2733)
- [At least one caught up node needs to be electable](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L2738)
- [Set the LeaderMode to kSteppingDown](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L1721)
- [Upon a successful stepdown, it yields locks held by prepared transactions](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_step_up_step_down.cpp#L444)

### Unconditional

Stepdowns can also occur for the following reasons:

- If the primary learns of a higher term
- Liveness timeout: If a primary stops being able to transitively communicate with a majority of
  nodes. The primary does not need to be able to communicate directly with a majority of nodes. If
  primary A can’t communicate with node B, but A can communicate with C which can communicate with B,
  that is okay. If you consider the minimum spanning tree on the cluster where edges are connections
  from nodes to their sync source, then as long as the primary is connected to a majority of nodes, it
  will stay primary.
- Force reconfig via the `replSetReconfig` command
- Force reconfig via heartbeat: If we learn of a newer config through heartbeats, we will
  schedule a replica set config change.

During unconditional stepdown, we do not check preconditions before attempting to step down. Similar
to conditional stepdowns, we must kill any conflicting user/system operations before acquiring the
RSTL and yield locks of prepared transactions following a successful stepdown.

#### Code references

- [Stepping down on learning of a higher term](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L5491-L5496)
- [Liveness timeout checks](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/topology_coordinator.cpp#L1236-L1249)
- [Stepping down on liveness timeout](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L479)
- [ReplSetReconfig command class](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/repl_set_commands.cpp#L431)
- [Stepping on reconfig](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L3873)
- [Stepping down on heartbeat](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L908)

### Concurrent Stepdown Attempts

It is possible to have concurrent conditional and unconditional stepdown attempts. In this case,
the unconditional stepdown will supercede the conditional stepdown, which causes the conditional
stepdown attempt to fail.

Because concurrent unconditional stepdowns can cause conditional stepdowns to fail, we stop
accepting writes once we confirm that we are allowed to step down. This way, if our stepdown
attempt fails, we can release the RSTL and allow secondaries to catch up without new writes coming
in.

We try to prevent concurrent conditional stepdown attempts by setting `_leaderMode` to
`kSteppingDown` in the `TopologyCoordinator`. By tracking the current stepdown state, we prevent
another conditional stepdown attempt from occurring, but still allow unconditional attempts to
supersede.

# Rollback: Recover To A Timestamp (RTT)

Rollback is the process whereby a node that diverges from its sync source gets back to a consistent
point in time on the sync source's branch of history.

Situations that require rollback can occur due to network partitions. Consider a scenario where a
secondary can no longer hear from the primary and subsequently runs for an election. We now have
two primaries that can both accept writes, creating two different branches of history (one of the
primaries will detect this situation soon and step down). If the smaller half, meaning less than a
majority of the set, accepts writes during this time, those writes will be uncommitted. A node with
uncommitted writes will roll back its changes and roll forward to match its sync source. Note that a
rollback is not necessary if there are no uncommitted writes.

As of 4.0, Replication supports the [`Recover To A Timestamp`](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/rollback_impl.h#L158)
algorithm (RTT), in which a node recovers to a consistent point in time and applies operations until
it catches up to the sync source's branch of history. RTT uses the WiredTiger storage engine to
recover to a [`stable_timestamp`](#replication-timestamp-glossary), which is the highest timestamp
at which the storage engine can take a checkpoint. This can be considered a consistent, majority
committed point in time for replication and storage.

A node goes into rollback when its [last fetched OpTime is greater than its sync source's lastApplied OpTime, but it is in a lower term](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/oplog_fetcher.cpp#L1019-L1024).
In this case, the `OplogFetcher` will return an empty batch and fail with an `OplogStartMissing` error,
which [`BackgroundSync` interprets as needing to rollback](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/bgsync.cpp#L600-L603).

During [rollback](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L175),
nodes first [transition to the `ROLLBACK`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L194)
state and kill all user operations to ensure that we can successfully acquire [the RSTL](#replication-state-transition-lock).
Reads are prohibited while we are in the `ROLLBACK` state.

We then [find the `common point`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L217)
between the rolling back node and the sync source node. The `common point` is the OpTime after which
the nodes' oplogs start to differ. During this step, we keep track of the operations that are rolled back up
until the `common point` and update necessary data structures. This includes metadata that we may
write out to rollback files and and use to roll back collection fast-counts. Then, we [increment](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L237)
the Rollback ID (RBID), a monotonically increasing number that is incremented every time a rollback
occurs. We can use the RBID to check if a rollback has occurred on our sync source since the
baseline RBID was set. [Note that the RBID is stored durably on disk](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/storage_interface_impl.cpp#L181).

We then [wait for background index builds to complete before entering rollback](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L548).
We aren't sure exactly what issues may arise when index builds run concurrently with rolling back to a
stable timestamp, but rather than dealing with the complexity we took the conservative approach of waiting
for all index builds before beginning rollback.

Now, we enter the [data modification section](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L551) of the rollback algorithm, which begins with
aborting prepared transactions and ends with reconstructing them at the end. If we fail at any point
during this phase, we must terminate the rollback attempt because we cannot safely recover.

Before we actually recover to the `stableTimestamp`, we must abort the storage transaction of any
prepared transaction. In doing so, we release any resources held by those transactions and
invalidate any in-memory state we recorded.

If `createRollbackDataFiles` was set to `true` (the default), we begin writing rollback files for
our rolled back documents. It is important that we do this after we abort any prepared transactions
in order to avoid unnecessary prepare conflicts when trying to read documents that were modified by
those transactions, which must be aborted for rollback anyway. Finally, if we have rolled back any
operations, we invalidate all sessions on this server.

Now, we are ready to tell the storage engine to [recover to the last `stable_timestamp`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/rollback_impl.cpp#L585-L586).
Upon success, the storage engine restores the data reflected in the database to the data reflected at the
last `stable_timestamp`. This does not, however, revert the oplog. In order to revert the oplog,
rollback must remove all oplog entries after the `common point`. This is called the truncate point
and is written into the `oplogTruncateAfterPoint` document. Now, the recovery process knows where to
truncate the oplog on the rollback node.

Before truncating the oplog, the rollback procedure will also make sure that session information in
`config.transactions` table is consistent with the `stableTimestamp`. As part of [vectored inserts](https://github.com/mongodb/mongo/blob/1182fa8c9889c88c22a5eb934d99e098456d0cbc/src/mongo/db/catalog/README.md#vectored-inserts)
and secondary oplog application of retryable writes, updates to the same session entry in the
`config.transactions` table will be coalesced as a single update when applied in the same batch. In
other words, we will only apply the last update to a session entry in a batch. However, if
the `stableTimestamp` refers to a point in time that is before the last update, it is possible to
lose the session information that was never applied as part of the coalescing.

As an example, consider the following:

1.  During a single batch of secondary oplog application:
    i). User data write for stmtId=0 at t=10.
    ii). User data write for stmtId=1 at t=11.
    iii). User data write for stmtId=2 at t=12.
    iv). Session txn record write at t=12 with stmtId=2 as lastWriteOpTime. In particular, no
    session txn record write for t=10 with stmtId=0 as lastWriteOpTime or for t=11 with stmtId=1 as lastWriteOpTime because they were coalseced by the [SessionUpdateTracker](https://github.com/mongodb/mongo/blob/9d601c939bca2a4304dca2d3c8abd195c1f070af/src/mongo/db/repl/session_update_tracker.cpp#L217-L221).
2.  Rollback to stable timestamp t=10.
3.  The session txn record won't exist with stmtId=0 as lastWriteOpTime (because the write was
    entirely skipped by oplog application) despite the user data write for stmtId=0 being reflected
    on-disk. Without any fix, this allows stmtId=0 to be re-executed by this node if it became primary.

As a solution, we traverse the oplog to find the last completed retryable write statements that occur before or at the `stableTimestamp`, and use this information to restore the `config.transactions`
table. More specifically, we perform a forward scan of the oplog starting from the first entry
greater than the `stableTimestamp`. For any entries with a non-null `prevWriteOpTime` value less
than or equal to the `stableTimestamp`, we create a `SessionTxnRecord` and perform an untimestamped
write to the `config.transactions` table. We must do an untimestamped write so that it will not be
rolled back on recovering to the `stableTimestamp` if we were to crash. Finally, we take a stable checkpoint so that these restoration writes are persisted to disk before truncating the oplog.

During the last few steps of the data modification section we run through the oplog recovery process,
which truncates the oplog after the `common point` and applies all oplog entries through the end of the sync source's oplog. See the
[Startup Recovery](#startup-recovery) section for more information on truncating the oplog and
applying oplog entries.

The last thing we do before exiting the data modification section is
[reconstruct prepared transactions](#recovering-prepared-transactions). We must also restore their
in-memory state to what it was prior to the rollback in order to fulfill the durability guarantees
of prepared transactions.

At this point, the lastApplied and durable OpTimes still point to the divergent branch of history,
so we must update them to be at the top of the oplog (the latest entry in the oplog), which should
be the `common point`.

Now, we can trigger the rollback `OpObserver` and notify any external subsystems that a rollback has
occurred. For example, the config server must update its shard registry in order to make sure it
does not have data that has just been rolled back. Finally, we log a summary of the rollback process
and transition to the `SECONDARY` state. This transition must succeed if we ever entered the
`ROLLBACK` state in the first place. Otherwise, we shut down.

# Initial Sync

Initial sync is the process that we use to add a new node to a replica set. Initial sync is
initiated by the `ReplicationCoordinator` and done in a registered subclass of
[**`InitialSyncerInterface`**](./initial_syncer_interface.h). The method used is specified by the server parameter `initialSyncMethod`.

There are currently two initial sync methods implemented, [**Logical Initial Sync**](#logical-initial-sync) (the default)
and File Copy Based Initial Sync, which is available only in MongoDB Enterprise Server.
If a method other than [**Logical Initial Sync**](#logical-initial-sync) is used,
and initial sync fails with `InvalidSyncSource`, a logical initial sync is attempted; this
fallback is also handled by the `ReplicationCoordinator`.

When a node begins initial sync, it goes into the `STARTUP2` state. `STARTUP` is reserved for the
time before the node has loaded its local configuration of the replica set.

## Initial Sync Semantics

Nodes in initial sync do not contribute to write concern acknowledgment. While in a `STARTUP2`
state, a node will not send any `replSetUpdatePosition` commands to its sync source. It will also
have the `lastAppliedOpTime` and `lastDurableOpTime` set to null in heartbeat responses. The
combined effect of this is that the primary of the replica set will not receive updates about the
initial syncing node's progress, and will thus not be able to count that member towards the
acknowledgment of writes.

In a similar vein, we prevent new members from voting (or increasing the number of nodes needed
to commit majority writes) until they have successfully completed initial sync and transitioned
to `SECONDARY` state. This is done as follows: whenever a new voting node is added to the set, we
internally rewrite its `MemberConfig` to have a special [`newlyAdded=true`](https://github.com/mongodb/mongo/blob/80f424c02df47469792917673ab7e6dd77b01421/src/mongo/db/repl/member_config.idl#L75-L81)
field. This field signifies that this node is temporarily non-voting and should thus be excluded
from all voter checks or counts. Once the replica set primary receives a heartbeat response from
the member stating that it is either in `SECONDARY`, `RECOVERING`, or `ROLLBACK` state, that primary
schedules an automatic reconfig to remove the corresponding `newlyAdded` field. Note that we filter
that field out of `replSetGetStatus` responses, but it is always visible in the config stored on
disk.

# Logical Initial Sync

Logical initial sync is the default initial sync method, implemented by
[**`InitialSyncer`**](./initial_syncer.h).

At a high level, there are two phases to initial sync: the [**data clone phase**](#data-clone-phase)
and the [**oplog application phase**](#oplog-application-phase). During the data clone phase, the
node will copy all of another node's data. After that phase is completed, it will start the oplog
application phase where it will apply all the oplog entries that were written since it started
copying data. Finally, it will reconstruct any transactions in the prepared state.

Before the data clone phase begins, the node will do the following:

1. Set the initial sync flag to record that initial sync is in progress and make it durable. If a
   node restarts while this flag is set, it will restart initial sync even though it may already
   have data because it means that initial sync didn't complete. We also check this flag to prevent
   reading from the oplog while initial sync is in progress.
2. [Reset the in-memory FCV to `kUnsetDefaultLastLTSBehavior`.](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/initial_syncer.cpp#L695). This is to ensure compatibility between the sync source and sync
   target. If the sync source is actually in a different feature compatibility version, we will find
   out when we clone from the sync source.
3. Find a sync source.
4. Drop all of its data except for the local database and recreate the oplog.
5. Get the Rollback ID (RBID) from the sync source to ensure at the end that no rollbacks occurred
   during initial sync.
6. Query its sync source's oplog for its latest OpTime and save it as the
   `defaultBeginFetchingOpTime`. If there are no open transactions on the sync source, this will be
   used as the `beginFetchingTimestamp` or the timestamp that it begins fetching oplog entries from.
7. Query its sync source's transactions table for the oldest starting OpTime of all active
   transactions. If this timestamp exists (meaning there is an open transaction on the sync source)
   this will be used as the `beginFetchingTimestamp`. If this timestamp doesn't exist, the node will
   use the `defaultBeginFetchingOpTime` instead. This will ensure that even if a transaction was
   started on the sync source after it was queried for the oldest active transaction timestamp, the
   syncing node will have all the oplog entries associated with an active transaction in its oplog.
8. Query its sync source's oplog for its lastest OpTime. This will be the `beginApplyingTimestamp`,
   or the timestamp that it begins applying oplog entries at once it has completed the data clone
   phase. If there was no active transaction on the sync source, the `beginFetchingTimestamp` will
   be the same as the `beginApplyingTimestamp`.
9. [Set the in-memory FCV to the sync source's FCV.](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/initial_syncer.cpp#L1165).
   This is because during the cloning phase, we do expect to clone the sync source's
   "admin.system.version" collection eventually (which contains the FCV document), but we can't
   guarantee that we will clone "admin.system.version" first. Setting the in-memory FCV value to the
   sync source's FCV first will ensure that we clone collections using the same FCV as the sync
   source. However, we won't persist the FCV to disk nor will we update our minWireVersion until we
   clone the actual document.
10. Create an `OplogFetcher` and start fetching and buffering oplog entries from the sync source
    to be applied later. Operations are buffered to a collection so that they are not limited by the
    amount of memory available.

## Data clone phase

The new node then begins to clone data from its sync source. The `InitialSyncer` constructs an
[`AllDatabaseCloner`](https://github.com/mongodb/mongo/blob/r4.3.2/src/mongo/db/repl/all_database_cloner.h)
that's used to clone all of the databases on the upstream node. The `AllDatabaseCloner` asks the
sync source for a list of its databases and then for each one it creates and runs a
[`DatabaseCloner`](https://github.com/mongodb/mongo/blob/r4.3.2/src/mongo/db/repl/database_cloner.h)
to clone that database. Each `DatabaseCloner` asks the sync source for a list of its collections and
for each one creates and runs a
[`CollectionCloner`](https://github.com/mongodb/mongo/blob/r4.3.2/src/mongo/db/repl/collection_cloner.h)
to clone that collection. The `CollectionCloner` calls `listIndexes` on the sync source and creates
a
[`CollectionBulkLoader`](https://github.com/mongodb/mongo/blob/r4.3.2/src/mongo/db/repl/collection_bulk_loader.h)
to create all of the indexes in parallel with the data cloning. The `CollectionCloner` then uses an
**exhaust cursor** to run a `find` request on the sync source for each collection, inserting the
fetched documents each time, until it fetches all of the documents. Instead of explicitly needing to
run a `getMore` on an open cursor to get the next batch, exhaust cursors make it so that if the
`find` does not exhaust the cursor, the sync source will keep sending batches until there are none
left.

The cloners are resilient to transient errors. If a cloner encounters an error marked with the
`RetriableError` label in
[`error_codes.yml`](https://github.com/mongodb/mongo/blob/r4.3.2/src/mongo/base/error_codes.yml), it
will retry whatever network operation it was attempting. It will continue attempting to retry for a
length of time set by the server parameter `initialSyncTransientErrorRetryPeriodSeconds`, after
which it will consider the failure permanent. A permanent failure means it will choose a new sync
source and retry all of initial sync, up to a number of times set by the server parameter
`numInitialSyncAttempts`. One notable exception, where we do not retry the entire operation, is for
the actual querying of the collection data. For querying, we use a feature called **resume
tokens**. We set a flag on the query: `$_requestResumeToken`. This causes each batch we receive
from the sync source to contain an opaque token which indicates our current position in the
collection. After storing a batch of data, we store the most recent resume token in a member
variable of the `CollectionCloner`. Then, when retrying we provide this resume token in the query,
allowing us to avoid having to re-fetch the parts of the collection we have already stored.

The `initialSyncTransientErrorRetryPeriodSeconds` is also used to control retries for the oplog
fetcher and all network operations in initial sync which take place after the data cloning has
started.

As of v4.4, initial syncing a node with [two-phase index builds](https://github.com/mongodb/mongo/blob/0a7641e69031fcfdf25a1780a3b62bca5f59d68f/src/mongo/db/catalog/README.md#replica-set-index-builds)
will immediately build all ready indexes from the sync source and setup the index builder threads
for any unfinished index builds.
[See here](https://github.com/mongodb/mongo/blob/85d75907fd12c2360cf16b97f941386f343ca6fc/src/mongo/db/repl/collection_cloner.cpp#L247-L301).

This is necessary to avoid a scenario where the primary node cannot satisfy the index builds commit
quorum if it depends on the initial syncing nodes vote. Prior to this, initial syncing nodes would
start the index build when they came across the `commitIndexBuild` oplog entry, which is only
observable once the index builds commit quorum has been satisfied.
[See this test for an example](https://github.com/mongodb/mongo/blob/f495bdead326a06a76f8a980e44092deb096a21d/jstests/noPassthrough/commit_quorum_does_not_hang_with_initial_sync.js).

## Oplog application phase

After the cloning phase of initial sync has finished, the oplog application phase begins. The new
node first asks its sync source for its lastApplied OpTime and this is saved as the
`stopTimestamp`, the oplog entry it must apply before it's consistent and can become a secondary. If
the `beginFetchingTimestamp` is the same as the `stopTimestamp`, then it indicates that there are no
oplog entries that need to be written to the oplog and no operations that need to be applied. In
this case, the node will seed its oplog with the last oplog entry applied on its sync source and
finish initial sync.

Otherwise, the new node iterates through all of the buffered operations, writes them to the oplog,
and if their timestamp is after the `beginApplyingTimestamp`, applies them to the data on disk.
Oplog entries continue to be fetched and added to the buffer while this is occurring. One thing to
note is that the oplog writes are not performed by the `OplogWriter` thread like [steady state
replication](#oplog-entry-persistence) but the initial sync thread pool, so it still needs to set
the [`oplogTruncateAfterPoint`](#replication-timestamp-glossary) to the node's [last written optime](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/oplog_writer_impl.cpp#L268)
(before this batch) to aid in [startup recovery](#startup-recovery) if the node shuts down in the
middle of writing entries to the oplog. After writing the batch, it will reset the
`oplogTruncateAfterPoint` to null.

One notable exception is that the node will not apply `prepareTransaction` oplog entries. Similar
to how we reconstruct prepared transactions in startup and rollback recovery, we will update the
transactions table every time we see a `prepareTransaction` oplog entry. Because the nodes wrote
all oplog entries starting at the `beginFetchingTimestamp` into the oplog, the node will have all
the oplog entries it needs to
[reconstruct the state for all prepared transactions](#recovering-prepared-transactions) after the
oplog application phase is done.

## Idempotency concerns

Some of the operations that are applied may already be reflected in the data that was cloned since
we started buffering oplog entries before the collection cloning phase even started. Consider the
following:

1. Start buffering oplog entries
2. Insert `{a: 1, b: 1}` to collection `foo`
3. Insert `{a: 1, b: 2}` to collection `foo`
4. Drop collection `foo`
5. Recreate collection `foo`
6. Create unique index on field `a` in collection `foo`
7. Clone collection `foo`
8. Start applying oplog entries and try to insert both `{a: 1, b: 1}` and `{a: 1, b: 2}`

As seen here, there can be operations on collections that have since been dropped or indexes could
conflict with the data being added. As a result, many errors that occur here are ignored and assumed
to resolve themselves, such as `DuplicateKey` errors (like in the example above).

## Finishing initial sync

The oplog application phase concludes when the node applies an oplog entry at `stopTimestamp`. The
node checks its sync source's Rollback ID to see if a rollback occurred and if so, restarts initial
sync. Otherwise, the `InitialSyncer` will begin tear down.

It will register the node's [`lastApplied`](#replication-timestamp-glossary) OpTime with the storage
engine to make sure that all oplog entries prior to that will be visible when querying the oplog.
After that it will reconstruct all prepared transactions. The node will then clear the initial sync
flag and tell the storage engine that the [`initialDataTimestamp`](#replication-timestamp-glossary)
is the node's lastApplied OpTime. Finally, the `InitialSyncer` shuts down and the
`ReplicationCoordinator` starts steady state replication.

#### Code References

- [ReplicationCoordinator starts initial sync if the node is started up without any data](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L1026)
- [Follow this flowchart for initial sync call stack.](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/initial_syncer.h#L278)
- [Initial syncer uses AllDatabaseCloner/DatabaseCloner/CollectionCloner to clone data from sync source, where the state transition is defined in runStages().](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/base_cloner.cpp#L268)
- [AllDatabaseCloner creates and runs each DatabaseCloner in its post stage.](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/all_database_cloner.cpp#L263)
- [DatabaseCloner creates and runs each CollectionCloner in its post stage.](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/database_cloner.cpp#L137)
- [InitialSyncer uses RollbackChecker to check if there is a rollback on sync source during initial sync.](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/initial_syncer.cpp#L2014)
- [Set lastApplied OpTime as initialDataTimestamp to storage engine after initial sync finishes.](https://github.com/mongodb/mongo/blob/r6.2.0/src/mongo/db/repl/initial_syncer.cpp#L586-L590)
- [Start steady state replication after initial sync completes.](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L965)

# Reconfiguration

MongoDB replica sets consist of a set of members, where a _member_ corresponds to a single
participant of the replica set, identified by a host name and port. We refer to a _node_ as the
mongod server process that corresponds to a particular replica set member. A replica set
_configuration_ [consists](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_config.idl#L133-L135) of a list of members in a replica set along with some member specific
settings as well as global settings for the set. We alternately refer to a configuration as a
_config_, for brevity. Each member of the config has a [member
id](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/member_id.h#L42-L45), which is a
unique integer identifier for that member. A config is defined in the
[ReplSetConfig](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_config.h#L156)
class, which is serialized as a BSON object and stored durably in the `local.system.replset`
collection on each replica set node.

## Initiation

When the mongod processes for members of a replica set are first started, they have [no configuration
installed](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L637-L644) and they do not communicate with each other over the network or replicate any data. To
initialize the replica set, an initial config must be provided via the [`replSetInitiate`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_commands.cpp#L332-L334) command, so
that nodes know who the other members of the replica set are. Upon receiving this command, which can
be run on any node of an uninitialized set, a node [validates](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L729-L730) and [installs](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L559-L564) the specified config. It
then establishes connections to and begins sending heartbeats to the other nodes of the replica set
contained in the configuration it installed. Configurations are propagated between nodes [via
heartbeats](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl_heartbeat.cpp#L470-L473), which is how nodes in the replica set will receive and install the initial config.

## Reconfiguration Behavior

To update the current configuration, a client may execute the [`replSetReconfig`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_commands.cpp#L424-L426) command with the
new, desired config. Reconfigurations [can be run
](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_commands.cpp#L446-L448)in
_safe_ mode or in _force_ mode. We alternately refer to reconfigurations as _reconfigs_, for
brevity. Safe reconfigs, which are the default, can only be run against primary nodes and ensure the
replication safety guarantee that majority committed writes will not be rolled back. Force reconfigs
can be run against either a primary or secondary node and their usage may cause the rollback of
majority committed writes. Although force reconfigs are unsafe, they exist to allow users to salvage
or repair a replica set where a majority of nodes are no longer operational or reachable.

### Safe Reconfig Protocol

The safe reconfiguration protocol implemented in MongoDB shares certain conceptual similarities with
the "single server" reconfiguration approach described in Section 4 of the [Raft PhD
thesis](https://web.stanford.edu/~ouster/cgi-bin/papers/OngaroPhD.pdf), but was designed with some
differences to integrate with the existing, heartbeat-based reconfig protocol more easily.

Note that in a static configuration, the safety of the Raft protocol depends on the fact that any
two quorums (i.e. majorities) of a replica set have at least one member in common i.e. they satisfy
the _quorum overlap_ property. For any two arbitrary configurations, however, this is not the case.
So, extra restrictions are placed on how nodes are allowed to move between configurations. First,
all safe reconfigs enforce a [single node
change](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_config_checks.cpp#L101-L109)
condition, which requires that no more than a single voting node is added or removed in a single
reconfig. Any number of non voting nodes can be added or removed in a single reconfig. This
constraint ensures that any adjacent configs satisfy quorum overlap. You can see a justification of
why this is true in the Raft thesis section referenced above.

Even though the single node change condition ensures quorum overlap between two adjacent configs,
quorum overlap may not always be ensured between configs on all nodes of the system, so there are
two additional constraints that must be satisfied before a primary node can install a new
configuration:

1. **[Config
   Replication](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L3963-L3966)**:
   The current config, C, must be installed on at least a majority of voting nodes in C.
2. **[Oplog
   Commitment](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L3998-L4005)**:
   Any oplog entries that were majority committed in the previous config, C0, must be replicated to at
   least a majority of voting nodes in the current config, C1.

Condition 1 ensures that any configs earlier than C can no longer independently form a quorum to
elect a node or commit a write. Condition 2 ensures that committed writes in any older configs are
now committed by the rules of the current configuration. This guarantees that any leaders elected in
a subsequent configuration will contain these entries in their log upon assuming role as leader.
When both conditions are satisfied, we say that the current config is _committed_.

We wait for both of these conditions to become true at the
[beginning](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_commands.cpp#L450-L466)
of the `replSetReconfig` command, before installing the new config. Satisfaction of these conditions
before transitioning to a new config is fundamental to the safety of the reconfig protocol. After
satisfying these conditions and installing the new config, we also wait for condition 1 to become
true of the new config at the
[end](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_commands.cpp#L471-L478)
of the reconfig command. This waiting ensures that the new config is installed on a majority of
nodes before reconfig returns success, but it is not strictly necessary for guaranteeing safety. If
it fails, an error will be returned, but the new config will have already been installed and can
begin to propagate. On a subsequent reconfig, we will still ensure that both safety conditions are
satisfied before installing the next config. By waiting for config replication at the end of the
reconfig command, however, we can make the waiting period shorter at the beginning of the next
reconfig, in addition to ensuring that the newly installed config will be present on a subsequent
primary.

Note that force reconfigs [bypass](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_commands.cpp#L453) all checks of condition 1 and 2, and they [do not enforce](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_config_checks.cpp#L450-L458) the single
node change condition.

### Config Ordering and Elections

As mentioned above, configs are propagated between nodes via heartbeats. To do this properly, nodes
must have some way of determining if one config is "newer" than another. Each configuration has a
`term` and `version` field, and configs are totally ordered by the [`(version,
term)`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/repl_set_config.h#L51-L56)
pair, where `term` is compared first, and then `version`, analogous to the rules for optime
comparison. The `term` of a config is the term of the primary that originally created that config,
and the `version` is a [monotonically increasing number](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L4065) assigned to each config. When executing a
reconfig, the version of the new config must be greater than the version of the current config. If
the `(version, term)` pair of config A is greater than that of config B, then it is considered
"newer" than config B. If a node hears about a newer config via a heartbeat from another node, it
will [schedule a
heartbeat](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L5364-L5388)
to fetch the config and
[install](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/topology_coordinator.cpp#L1004-L1006)
it locally.

Note that force reconfigs set the new config's term to an [uninitialized term
value](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/optime.h#L58-L59). When we
compare two configs, if either of them has an uninitialized term value, then we only consider config
versions for comparison. A force reconfig also [increments the
version](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L3442-L3449)
of the current config by a large, random number. This makes it very likely that the force config
will be "newer" than any other config in the system.

Config ordering also affects voting behavior. If a replica set node is a candidate for election in
config `(vc, tc)`, then a prospective voter with config `(v, t)` will only cast a vote for the
candidate if `(vc, tc) >= (v, t)`. For a description of the complete voting behavior, see the
[Elections](#Elections) section.

### Formal Specification

For more details on the safe reconfig protocol and its behaviors, refer to the [TLA+
specification](https://github.com/mongodb/mongo/tree/master/src/mongo/tla_plus/MongoReplReconfig)
or the [paper on MongoDB's reconfig protocol](https://arxiv.org/abs/2102.11960), written in part by replication engineers.
It defines two main invariants of the protocol, ElectionSafety and NeverRollbackCommitted,
which assert, respectively, that no two leaders are elected in the same term and that majority
committed writes are never rolled back.

# Startup Recovery

**Startup recovery** is a node's process for putting both the oplog and data into a consistent state
during startup (and happens while the node is in the `STARTUP` state). If a node has an empty or
non-existent oplog, or already has the initial sync flag set when starting up, then it will skip
startup recovery and go through [initial sync](#initial-sync) instead.

If the node already has data, it will go through
[startup recovery](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/replication_recovery.h#L44-L48).
It will first [get the **recovery timestamp**](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/replication_recovery.cpp#L469-L471)
from the storage engine, which is the timestamp through
which changes are reflected in the data at startup and the timestamp used to set the
[`initialDataTimestamp`](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h#L504-L506).
The recovery timestamp will be a `stable_timestamp` so that the node
recovers from a **stable checkpoint**, which is a durable view of the data at a particular timestamp.
It should be noted that due to journaling, the oplog and many collections in the local database are
an exception and are up-to-date at startup rather than reflecting the recovery timestamp.

If a node went through an unclean shutdown, then it might have been in the middle of applying
parallel writes. Each write is associated with an oplog entry. Primaries perform writes in parallel,
and batch application applies oplog entries in parallel. Since these operations are done in
parallel, they can cause temporary gaps in the oplog from entries that are not yet written, called
**oplog holes**. A node can crash while there are still **oplog holes** on disk.

During startup, a node will not be able to tell which oplog entries were successfully persisted in
the oplog and which were uncommitted on disk and disappeared. It also may have failed before writing
some oplog entries from memory to disk during secondary oplog application. Since a primary doesn't wait
for oplog entries to be durable before replicating them to secondaries, it may be unknowingly missing
oplog entries that the secondaries already replicated; or a secondary may lose oplog entries that it
thought it had already replicated. This would make the recently crashed node inconsistent with the
rest its replica set. To fix this, after getting the recovery timestamp, the node will
[truncate](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/replication_recovery.cpp#L474)
its oplog to a point that it can guarantee does not have any oplog holes using the
[`oplogTruncateAfterPoint`](#replication-timestamp-glossary) document. This document is journaled
and untimestamped so that it will reflect information more recent than the latest stable checkpoint
even after a shutdown.

The `oplogTruncateAfterPoint` can be set in two scenarios. The first is during
[oplog batch application](#oplog-entry-application). Before writing a batch of oplog entries to the
oplog, the node will [set the `oplogTruncateAfterPoint` to the `lastWritten` timestamp](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/oplog_writer_impl.cpp#L268).
If the node shuts down before it finishes writing the batch, then during startup recovery the node will truncate
the oplog back to the point saved before the batch application began. If the node successfully
finishes writing the batch to the oplog, it will
[reset the `oplogTruncateAfterPoint` to null](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/oplog_applier_impl.cpp#L499)
since there are no oplog holes and the oplog will not need to be truncated if the node restarts.

The second scenario for setting the `oplogTruncateAfterPoint` is while primary. A primary allows
secondaries to replicate one of its oplog entries as soon as there are no oplog holes in-memory
behind the entry. However, secondaries do not have to wait for the oplog entry to make it to disk
on the primary nor for there to be no holes behind it on disk on the primary. Therefore, some
already replicated writes may disappear from the primary if the primary crashes. The primary will
continually [update the `oplogTruncateAfterPoint`](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L1378-L1381)
in order to track and forward the no oplog holes
point on disk, in case of an unclean shutdown. Then startup recovery can take care of any oplog
inconsistency with the rest of the replica set.

After truncating the oplog, the node will see if the recovery timestamp [differs from the top of the
newly truncated oplog](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/replication_recovery.cpp#L660-L673).
If it does, this means that there are oplog entries that must be applied to
make the data consistent with the oplog. The [node will apply](https://github.com/mongodb/mongo/blob/r6.0.0/src/mongo/db/repl/replication_recovery.cpp#L684)
all the operations starting at the
recovery timestamp through the top of the oplog (the latest entry in the oplog). The one exception
is that it will not apply `prepareTransaction` oplog entries. Similar to [how a node reconstructs](#recovering-prepared-transactions)
prepared transactions during initial sync and rollback, the node will update the transactions table
every time it sees a `prepareTransaction` oplog entry. Once the node has finished applying all the
oplog entries through the top of the oplog, it will
[reconstruct](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L679-L682)
all transactions still in the prepare state.

Finally, the node will [finish loading](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L691)
the replica set configuration, [set its `lastWritten`, `lastApplied` and
`lastDurable`](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L810-L812) timestamps
to the top of the oplog (the latest entry in the oplog) and start steady state replication.

## Recover from Unstable Checkpoint

We may not have a recovery timestamp if we need to recover from an **unstable checkpoint**. MongoDB
takes unstable checkpoints by setting the [`initialDataTimestamp`](#replication-timestamp-glossary)
to the `kAllowUnstableCheckpointsSentinel`. Recovery from an unstable checkpoint replays the oplog
from [the "appliedThrough" value in the `minValid` document](https://github.com/mongodb/mongo/blob/d8f3983e6976589cd9fa47c254cae015d9dbbd1a/src/mongo/db/repl/replication_recovery.cpp#L550-L563)
to [the end of oplog](https://github.com/mongodb/mongo/blob/d8f3983e6976589cd9fa47c254cae015d9dbbd1a/src/mongo/db/repl/replication_recovery.cpp#L591-L594).
Therefore, when the last checkpoint is an unstable checkpoint, we must have a valid "appliedThrough"
reflected in that checkpoint so that replication recovery can run correctly in case the node
crashes. We transition from taking unstable checkpoints to stable checkpoints by setting a valid
`initialDataTimestamp`. The first stable checkpoint is taken
[when the stable timestamp is >= the `initialDataTimestamp` set](https://github.com/mongodb/mongo/blob/d8f3983e6976589cd9fa47c254cae015d9dbbd1a/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1928-L1934).
To avoid the confusion of having an "appliedThrough" conflicting with the stable recovery timestamp,
the "appliedThrough" is cleared after we set a valid `initialDataTimestamp`. This is safe
because we will no longer take unstable checkpoints from now on. This means that no unstable
checkpoint will be taken with the "appliedThrough" cleared and all future stable checkpoints are
guaranteed to be taken with the "appliedThrough" cleared. Therefore, if this node crashes before
the first stable checkpoint, it can safely recover from the last unstable checkpoint with a correct
appliedThrough value. Otherwise, if this node crashes after the first stable checkpoint is taken,
it can safely recover from a stable checkpoint (with a cleared "appliedThrough").

# Flow Control

The Flow Control mechanism aims to keep replica set majority committed lag less than or equal to a
configured maximum. The default value for this maximum lag is 10 seconds. The Flow Control mechanism
starts throttling writes on the primary once the majority committed replication lag reaches a
threshold percentage of the configured maximum. The mechanism uses a "ticket admission"-based
approach to throttle writes. With this mechanism, in a given period of 1 second, a fixed number of
"flow control tickets" is available. Operations must acquire a flow control ticket in order to
acquire a global IX lock to execute a write. Acquisition attempts that occur after this fixed number
has been granted will stall until the next 1 second period. Certain system operations circumvent the
ticket admission mechanism and are allowed to proceed even when there are no tickets available.

To address the possibility of this Flow Control mechanism causing indefinite stalls in
Primary-Secondary-Arbiter replica sets in which a majority cannot be established, the mechanism only
executes when read concern majority is enabled. Additionally, the mechanism can be disabled by an
admin.

Flow Control is configurable via several server parameters. Additionally, currentOp, serverStatus,
database profiling, and slow op log lines include Flow Control information.

## Flow Control Ticket Admission Mechanism

The ticket admission Flow Control mechanism allows a specified number of global IX lock acquisitions
every second. Most global IX lock acquisitions (except for those that explicitly circumvent Flow
Control) must first acquire a "Flow Control ticket" before acquiring a ticket for the lock. When
there are no more flow control tickets available in a one second period, remaining attempts to
acquire flow control tickets stall until the next period, when the available flow control tickets
are replenished. It should be noted that there is no "pool" of flow control tickets that threads
give and take from; an independent mechanism refreshes the ticket counts every second.

When the Flow Control mechanism refreshes available tickets, it calculates how many tickets it
should allow in order to address the majority committed lag.

The Flow Control mechanism determines how many flow control tickets to replenish every period based
on:

1. The current majority committed replication lag with respect to the configured target maximum
   replication lag
1. How many operations the secondary sustaining the commit point has applied in the last period
1. How many IX locks per operation were acquired in the last period

## Configurable constants

Criterion #2 determines a "base" number of flow control tickets to be used in the calculation. When
the current majority committed lag is greater than or equal to a certain configurable threshold
percentage of the target maximum, the Flow Control mechanism scales down this "base" number based on
the discrepancy between the two lag values. For some configurable constant 0 < k < 1, it calculates
the following:

`base * k ^ ((lag - threshold)/threshold) * fudge factor`

The fudge factor is also configurable and should be close to 1. Its purpose is to assign slightly
lower than the "base" number of flow control tickets when the current lag is close to the threshold.
Criterion #3 is then multiplied by the result of the above calculation to translate a count of
operations into a count of lock acquisitions.

When the majority committed lag is less than the threshold percentage of the target maximum, the
number of tickets assigned in the previous period is used as the "base" of the calculation. This
number is added to a configurable constant (the ticket "adder" constant), and the sum is multiplied
by another configurable constant (the ticket "multiplier" constant). This product is the new number
of tickets to be assigned in the next period.

When the Flow Control mechanism is disabled, the ticket refresher mechanism always allows one
billion flow control ticket acquisitions per second. The Flow Control mechanism can be disabled via
a server parameter. Additionally, the mechanism is disabled on nodes that cannot accept writes.

Criteria #2 and #3 are determined using a sampling mechanism that periodically stores the necessary
data as primaries process writes. The sampling mechanism executes regardless of whether Flow Control
is enabled.

## Oscillations

There are known scenarios in which the Flow Control mechanism causes write throughput to
oscillate. There is no known work that can be done to eliminate oscillations entirely for this
mechanism without hindering other aspects of the mechanism. Work was done (see SERVER-39867) to
dampen the oscillations at the expense of throughput.

## Throttling internal operations

The Flow Control mechanism throttles all IX lock acquisitions regardless of whether they are from
client or system operations unless they are part of an operation that is explicitly excluded from
Flow Control. Writes that occur as part of replica set elections in particular are excluded. See
SERVER-39868 for more details.

# Feature Compatibility Version

See the [FCV and Feature Flag README](FCV_AND_FEATURE_FLAG_README.md).

# System Collections

Much of mongod's configuration and state is persisted in "system collections" in the "admin"
database, such as `admin.system.version`, or the "config" database, such as `config.transactions`.
(These collections are both replicated. Unreplicated configuration and state is stored in the
"local" database.) The difference between "admin" and "config" for system collections is historical;
from now on when we invent a new system collection we will place it on "admin".

# Replication Timestamp Glossary

In this section, when we refer to the word "transaction" without any other qualifier, we are talking
about a storage transaction (aka [WriteUnitOfWork](https://github.com/mongodb/mongo/blob/00fbc981646d9e6ebc391f45a31f4070d4466753/src/mongo/db/storage/write_unit_of_work.h#L48)).
Transactions in the replication layer will be referred to as multi-document or prepared transactions.

**`all_durable`**: All transactions with timestamps earlier than the `all_durable` timestamp are
committed. This is the point at which the oplog has no gaps, which are created when we reserve
timestamps before executing the associated write (ex: [insert path](https://github.com/mongodb/mongo/blob/2ff8fff5b01eeda5722884c5fd104716117c9606/src/mongo/db/ops/write_ops_exec.cpp#L379)).
Since this timestamp is used to maintain the oplog visibility point, it is important that all
operations up to and including this timestamp are committed. This is so that we can replicate the
oplog without any gaps.  
This is calculated at the storage level and can be retrieved through [getAllDurableTimestamp](https://github.com/mongodb/mongo/blob/2ff8fff5b01eeda5722884c5fd104716117c9606/src/mongo/db/repl/storage_interface.h#L471).  
Contrary to what the name might imply, this timestamp does not indicate that all transactions
preceding it are durable on disk; rather, it solely signifies they are committed. Therefore,
replication consistently [maintains that](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L4981-L4998) `stable_timestamp` <= `all_durable`.

**`currentCommittedSnapshot`**: An optime maintained in `ReplicationCoordinator` that is used to
serve majority reads and is always guaranteed to be <= `lastCommittedOpTime`. This
is currently [set to the stable optime](hhttps://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L5085).
Since it is reset every time we recalculate the stable optime, it will also be up to date.

**`initialDataTimestamp`**: A timestamp used to indicate the timestamp at which history “begins”.
When a node comes out of initial sync, we inform the storage engine that the `initialDataTimestamp`
is the node's `lastApplied`.  
By setting this value to 0, it informs the storage engine to take unstable checkpoints. Stable
checkpoints can be viewed as timestamped reads that persist the data they read into a checkpoint.
Unstable checkpoints simply open a transaction and read all data that is currently committed at the
time the transaction is opened. They read a consistent snapshot of data, but the snapshot they read
from is not associated with any particular timestamp.

**`lastWritten`**: OpTime of the latest oplog entry that has been written to the `rs.oplog`
collection, though it is not necessary to be flushed to the journal. On primary, it is equal to the
`lastApplied` timestamp where they are updated together after a storage transaction commits, so it
can include an oplog hole. On secondary, `lastWritten` will be updated after the `OplogWriter`
writes a batch of oplog entries to `rs.oplog`, which happens before journal flushing and oplog
application. Therefore, `lastWritten` is guaranteed to be greater than or equal to both `lastApplied`
and `lastDurable`.

**`lastApplied`**: In-memory record of the latest applied oplog entry optime. On primaries, it may
lag behind the optime of the newest oplog entry that is visible in the storage engine because it is
updated after a storage transaction commits. On secondaries, lastApplied is only updated at the
completion of an oplog batch. `lastApplied` can include an oplog hole on primary since transactions
may not commit in order but won't include oplog holes on secondary.

**`lastCommittedOpTime`**: A node’s local view of the latest majority committed optime. Every time
we update this optime, we also recalculate the `stable_timestamp`. Note that the
`lastCommittedOpTime` can advance beyond a node's `lastApplied` if it has not yet replicated the
most recent majority committed oplog entry. For more information about how the `lastCommittedOpTime`
is updated and propagated, please see [Commit Point Propagation](#commit-point-propagation).

**`lastDurable`**: Optime of either the latest oplog entry (non-primary) or the latest no oplog
holes point (primary) that has been flushed to the journal. It is asynchronously updated by the
storage engine as new writes become durable. Default journaling frequency is 100ms.

**`minValid`**: Optime that indicates the point a node has to apply through for the data to be
considered consistent. This optime is set on the `minValid` document in
[`ReplicationConsistencyMarkers`](https://github.com/mongodb/mongo/blob/r4.2.0/src/mongo/db/repl/replication_consistency_markers.h),
which means that it will be persisted between restarts of a node.

**`oldest_timestamp`**: The earliest timestamp that the storage engine is guaranteed to have history
for. New transactions can never start a timestamp earlier than this timestamp. Since we advance this
as we advance the `stable_timestamp`, it will be less than or equal to the `stable_timestamp`.

**`oplogTruncateAfterPoint`**: Tracks the latest no oplog holes point. On primaries, it is updated
by the storage engine prior to flushing the journal to disk. During
[oplog batch application](#oplog-entry-application), it is set at the start of the batch and cleared
at the end of batch application. Startup recovery will use the `oplogTruncateAfterPoint` to truncate
the oplog back to an oplog point consistent with the rest of the replica set: other nodes may have
replicated in-memory data that a crashed node no longer has and is unaware that it lacks.

**`readConcernMajorityOpTime`**: Exposed in replSetGetStatus as “readConcernMajorityOpTime” but is
populated internally from the `currentCommittedSnapshot` timestamp inside `ReplicationCoordinator`.

**`stable_timestamp`**: The newest timestamp at which the storage engine is allowed to take a
checkpoint, which can be thought of as a consistent snapshot of the data. Replication informs the
storage engine of where it is safe to take its next checkpoint. This timestamp is guaranteed to be
majority committed (other than a specific caveat during restore noted below) so that RTT rollback
can use it.  
The calculation of this value in the replication layer occurs [here](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L4957-L5027).
The replication layer will [skip setting the stable timestamp](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L5048-L5062)
if it is earlier than the `initialDataTimestamp`, since data earlier than that timestamp may be
inconsistent.
During restore, we may proactively set the stable timestamp as we apply oplog batches even before
we set the `initialDataTimestamp`. This means that after startup recovery for restore (and for
File Copy Based Initial Sync), we cannot guarantee that the stable timestamp is actually majority
committed. However, this is safe because we do not allow rollbacks before the
`initialDataTimestamp` and when both FCBIS and startup recovery for restore complete, the
`initialDataTimestamp` will be equal to the stable timestamp.

#### Timestamps related to both prepared and non-prepared transactions:

- **`prepareTimestamp`**: The timestamp of the ‘prepare’ oplog entry for a prepared transaction. This
  is the earliest timestamp at which it is legal to commit the transaction. This timestamp is provided
  to the storage engine to block reads that are trying to read prepared data until the storage engines
  knows whether the prepared transaction has committed or aborted.

- **`commit oplog entry timestamp`**: The timestamp of the ‘commitTransaction’ oplog entry for a
  prepared transaction, or the timestamp of the ‘applyOps’ oplog entry for a non-prepared transaction.
  In a cross-shard transaction each shard may have a different commit oplog entry timestamp. This is
  guaranteed to be greater than the `prepareTimestamp`. When the `stable_timestamp` advances to this
  point, the transaction can’t be rolled-back; hence, it is referred to as the transaction's
  `durable_timestamp` in [WT](https://source.wiredtiger.com/develop/timestamp_txn_api.html).

- **`commitTimestamp`**: The timestamp at which we committed a multi-document transaction, referred
  to as `commit_timestamp` in [WT](https://source.wiredtiger.com/develop/timestamp_txn_api.html). This will
  be the `commitTimestamp` field in the `commitTransaction` oplog entry for a prepared transaction, or
  the timestamp of the ‘applyOps’ oplog entry for a non-prepared transaction. In a cross-shard
  transaction this timestamp is the same across all shards. The effects of the transaction are visible
  as of this timestamp. Note that `commitTimestamp` and the `commit oplog entry timestamp` are the
  same for non-prepared transactions because we do not write down the oplog entry until we commit the
  transaction. For a prepared transaction, we have the following guarantee: `prepareTimestamp` <=
  `commitTimestamp` <= `commit oplog entry timestamp`

# Replication state transitions

```mermaid
  graph TD
  STARTUP --> STARTUP2
  STARTUP2 --> RECOVERING
  RECOVERING --> SECONDARY
  SECONDARY <-- election --> PRIMARY
  PRIMARY --> REMOVED
  PRIMARY -- [5] --> RECOVERING
  SECONDARY <--> ROLLBACK
  ROLLBACK --[7] --> RECOVERING
  REMOVED -- [6] --> RECOVERING
  SECONDARY -- [4] --> MAINTENANCE(MAINTENANCE #91;2#93;)
  MAINTENANCE -- [3] --> SECONDARY
  STARTUP --> ARBITER
  ARBITER <--> REMOVED
  REMOVED <--> SECONDARY
  ROLLBACK --> REMOVED
  STARTUP --> REMOVED
  REMOVED <--> STARTUP2
  UNKNOWN(UNKNOWN #91;1#93;)
  DOWN(DOWN #91;1#93;)

  %% The following are invisible links and nodes used for formatting:
  REMOVED ~~~ UNKNOWN
  REMOVED ~~~ DOWN
  ROLLBACK ~~~ SPACER(" ") ~~~ REMOVED
  style SPACER height:0px;
```

- **`[1]`**: A node can never be in that state. One node can consider another to be in that state.
- **`[2]`**: Not an actual MemberState, RECOVERING + a [separate flag](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/bgsync.cpp#L421)
- **`[3]`**: With manual replSetMaintenance or when [switching sync source](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/bgsync.cpp#L485-L491)
- **`[4]`**: Too stale or manual replSetMaintenance
- **`[5]`**: When stepping down with [\_hasOnlyAuthErrorUpHeartbeats(...) returning true](https://github.com/mongodb/mongo/blob/r8.0.1/src/mongo/db/repl/topology_coordinator.cpp#L2739-L2741)
- **`[6]`**: [Code in ReplicationCoordinatorImpl::\_startDataReplication](https://github.com/mongodb/mongo/blob/c8ebdc8b2ef2379bba978ab688e2eda1ac702b15/src/mongo/db/repl/replication_coordinator_impl.cpp#L962-L963) allows it

# Non-replication subsystems dependent on replication state transitions.

The replication machinery provides two different APIs for mongod subsystems to receive notifications
about replication state transitions. The first, simpler API is the ReplicaSetAwareService interface.
The second, more sophisticated but also more prescriptive API is the PrimaryOnlyService interface.

## ReplicaSetAwareService interface

The ReplicaSetAwareService interface provides simple hooks to receive notifications on transitions
into and out of the Primary state. By extending ReplicaSetAwareService and overriding its virtual
methods, it is possible to get notified every time the current mongod node steps up or steps down.
Because the onStepUp and onStepDown methods of ReplicaSetAwareServices are called inline as part of
the stepUp and stepDown processes, while the RSTL is held, ReplicaSetAwareService subclasses should
strive to do as little work as possible in the bodies of these methods, and should avoid performing
blocking i/o, as all work performed in these methods delays the replica set state transition for the
entire node which can result in longer periods of write unavailability for the replica set.

## PrimaryOnlyService interface

The PrimaryOnlyService interface is more sophisticated than the ReplicaSetAwareService interface and
is designed specifically for services built on persistent state machines that must be driven to
conclusion by the Primary node of the replica set, even across failovers. Check out [this
document](../../../../docs/primary_only_service.md) for more information about PrimaryOnlyServices.
