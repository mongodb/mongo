# Sharding Internals

## Recommended prerequisite reading

A reader should be familiar with the
[**general concept**](https://docs.mongodb.com/manual/sharding/#sharding)
of horizontal scaling or "sharding", the
[**architecture of a MongoDB sharded cluster**](https://docs.mongodb.com/manual/sharding/#sharded-cluster),
and the concept of a
[**shard key in MongoDB**](https://docs.mongodb.com/manual/sharding/#shard-keys).

---

# Routing

There is an authoritative routing table stored on the config server replica set, and all nodes cache
the routing table in memory so that they can route requests to the shard(s) that own the
corresponding data.

## The authoritative routing table

The authoritative routing table is stored in a set of unsharded collections in the config database
on the config server replica set. The schemas of the relevant collections are:

* [**config.databases**](https://docs.mongodb.com/manual/reference/config-database/#config.databases)
* [**config.collections**](https://docs.mongodb.com/manual/reference/config-database/#config.collections)
* [**config.chunks**](https://docs.mongodb.com/manual/reference/config-database/#config.chunks)
* [**config.shards**](https://docs.mongodb.com/manual/reference/config-database/#config.shards)

#### Code references

* The
[**indexes created**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager.cpp#L295-L372)
on each of these collections for efficient querying

## The routing table cache

## Shard versioning and database versioning

## The shards list cache

## Replica set monitoring and host targeting

---

# Migrations

Data is migrated from one shard to another at the granularity of a single chunk.

It is also possible to move unsharded collections as a group by changing the primary shard of a
database. This uses a protocol similar to but less robust than the one for moving a chunk, so only
moving a chunk is discussed here.

## The live migration protocol
A chunk is moved from one shard to another by the moveChunk command. This command can be issued either manually or by the balancer. The live migration protocol consists of an exchange of messages between two shards, the donor and the recipient. This exchange is orchestrated by the donor shard in the [moveChunk command](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/move_chunk_command.cpp#L214) which follows a series of steps.

1. **Start the migration** - The ActiveMigrationsRegistry is [updated on the donor side](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/move_chunk_command.cpp#L138) to reflect that a specific chunk is being moved. This prevents any other chunk migrations from happening on this shard until the migration is completed. If an existing incoming or outgoing migration is in flight then the registration will fail and the migration will be aborted. If the inflight operation is for the same chunk then the the registration call will return an object that the moveChunk command can use to join the current operation.
1. **Start cloning the chunk** - After validating the migration parameters, the donor starts the migration process by sending the _recvChunkStart message to the recipient. This causes the recipient to then [initiate the transfer of documents](https://github.com/mongodb/mongo/blob/5c72483523561c0331769abc3250cf623817883f/src/mongo/db/s/migration_destination_manager.cpp#L955) from the donor. The initial transfer of documents is done by [repeatedly sending the _migrateClone command to the donor](https://github.com/mongodb/mongo/blob/5c72483523561c0331769abc3250cf623817883f/src/mongo/db/s/migration_destination_manager.cpp#L1042) and inserting the fetched documents on the recipient.
1. **Transfer queued modifications** - Once the initial batch of documents are copied, the recipient then needs to retrieve any modifications that have been queued up on the donor. This is done by  [repeatedly sending the _transferMods command to the donor](https://github.com/mongodb/mongo/blob/5c72483523561c0331769abc3250cf623817883f/src/mongo/db/s/migration_destination_manager.cpp#L1060-L1111). These are [inserts, updates and deletes](https://github.com/mongodb/mongo/blob/11eddfac181ff6ff9faf3e1d55c050373bc6fc24/src/mongo/db/s/migration_chunk_cloner_source_legacy.cpp#L534-L550) that occurred on the donor while the initial batch was being transferred.
1. **Wait for recipient to clone documents** - The donor [polls the recipient](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/migration_chunk_cloner_source_legacy.cpp#L984) to see when the transfer of documents has been completed or the timeout has been exceeded. This is indicated when the recipient returns a state of “steady” as a result of the _recvChunkStatus command.
1. **Enter the critical section** - Once the recipient has cloned the initial documents, the donor then [declares that it is in a critical section](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/migration_source_manager.cpp#L344). This indicates that the next operations must not be interrupted and will require recovery actions if they are interrupted. Writes to the donor shard will be suspended while the critical section is in effect. The mechanism to implement the critical section writes the ShardingStateRecovery document to store the minimum optime of the sharding config metadata. If this document exists on stepup it is used to update the optime so that the correct metadata is used.
1. **Commit on the recipient** - While in the critical section, the [_recvChunkCommit](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/migration_chunk_cloner_source_legacy.cpp#L360) command is sent to the recipient directing it to fetch any remaining documents for this chunk. The recipient responds by sending _transferMods to fetch the remaining documents while writes are blocked on the donor. Once the documents are transferred successfully, the _recvChunkCommit command returns it’s status to unblock the donor.
1. **Commit on the config server** - The donor sends the _configsvrCommitChunkMigration command to the config server. Before the command is sent, [reads are also suspended](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/migration_source_manager.cpp#L436) on the donor shard.

#### Code references
* [ActiveMigrationRegistry](https://github.com/mongodb/mongo/blob/9be1041342b666e979aaea483c2fdb929c801796/src/mongo/db/s/active_migrations_registry.h#L52) class
* [MigrationSourceManager](https://github.com/mongodb/mongo/blob/2c87953010c2c1ec2d39dc9a7dbbd5f7d49dab10/src/mongo/db/s/migration_source_manager.h#L70) class
* [MigrationDestinationManager](https://github.com/mongodb/mongo/blob/2c87953010c2c1ec2d39dc9a7dbbd5f7d49dab10/src/mongo/db/s/migration_destination_manager.h#L71) class
* [MigrationChunkClonerSourceLegacy](https://github.com/mongodb/mongo/blob/11eddfac181ff6ff9faf3e1d55c050373bc6fc24/src/mongo/db/s/migration_chunk_cloner_source_legacy.h#L82) class
* [ShardingStateRecovery](https://github.com/mongodb/mongo/blob/2c87953010c2c1ec2d39dc9a7dbbd5f7d49dab10/src/mongo/db/s/sharding_state_recovery.h#L47) class

## Orphaned range deletion

## Orphan filtering

## Replicating the orphan filtering table

---

# Auto-splitting and auto-balancing

Data may need to redistributed for many reasons, such as that a shard was added, a shard was
requested to be removed, or data was inserted in an imbalanced way.

The config server replica set durably stores settings for the maximum chunk size and whether chunks
should be automatically split and balanced.

## Auto-splitting

## Auto-balancing

---

# DDL operations

Indexes are not stored in the routing table, so a router forwards index operations to all shards
that own chunks for the collection.

Collections are always created as unsharded, meaning they are not stored in the routing table, so
a router forwards create collection requests directly to the primary shard for the database. A
router also forwards rename collection requests directly to the primary shard, since only renaming
unsharded collections is supported.

A router forwards all other DDL operations, such as dropping a database or sharding a collection,
to the config server primary. The config server primary serializes conflicting operations, and
either itself coordinates the DDL operation or hands off the coordination to a shard. Coordinating
the DDL operation involves applying the operation on the correct set of shards and updating the
authoritative routing table.

#### Code references
* Example of a DDL command (create indexes) that mongos
[**forwards to all shards that own chunks**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/commands/cluster_create_indexes_cmd.cpp#L83)
* Example of a DDL command (create collection) that mongos
[**forwards to the primary shard**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/commands/cluster_create_cmd.cpp#L128)
* Example of a DDL command (drop collection) mongos
[**forwards to the config server primary**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/commands/cluster_drop_cmd.cpp#L81-L82)
* Example of a DDL command (drop collection) the config server
[**coordinates itself**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/configsvr_drop_collection_command.cpp).
The business logic for most DDL commands that the config server coordinates lives in the
[**ShardingCatalogManager class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager.h#L86),
including the logic for
[**dropCollection**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager_collection_operations.cpp#L417).
However, note that the ShardingCatalogManager class also contains business logic to just commit some
operations that are otherwise coordinated by a shard.
* Example of a DDL command (shard collection) for which the config server
[**hands off coordination**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/configsvr_shard_collection_command.cpp)
to a shard. The business logic for such commands is in the shard's command body, such as the logic
for
[**shardCollection**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/shardsvr_shard_collection.cpp#L7830).

## Important caveats

### Database creation

There is no explicit command to create a database. When a router receives a write command, an entry
for the database is created in the config.databases collection if one doesn't already exist. Unlike
all other DDL operations, creating a database only involves choosing a primary shard (the shard with
the smallest total data size is chosen) and writing the database entry to the authoritative routing
table. That is, creating a database does not involve modifying any state on shards, since on shards,
a database only exists once a collection in it is created.

#### Code references
* Example of mongos
[**asking the config server to create a database**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/commands/cluster_create_cmd.cpp#L116)
if needed

### Retrying internally

DDL operations often involve multiple hops between nodes. Generally, if a command is idempotent on
the receiving node, the sending node will retry it upon receiving a retryable error, such as a
network or NotMaster error. There are some cases where the sending node retries even though the
command is not idempotent, such as in shardCollection. In this case, the receiving node may return
ManualInterventionRequired if the first attempt failed partway.

#### Code references
* Example of a DDL command (shard collection)
[**failing with ManualInterventionRequired**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/shardsvr_shard_collection.cpp#L129)

### Serializing conflicting operations

The concurrency control scheme has evolved over time and involves several different locks.

Distributed locks are locks on a string resource, typically a database name or collection name. They
are acquired by doing a majority write to a document in the `config.locks` collection on the config
servers. The write includes the identity of the process acquiring the lock. The process holding a
distributed lock must also periodically "ping" (update) a document in the `config.lockpings`
collection on the config servers containing its process id. If the process's document is not pinged
for 15 minutes or more, the process's distributed locks are allowed to be "overtaken" by another
process. Note that this means a distributed lock can be overtaken even though the original process
that had acquired the lock continues to believe it owns the lock. See
[**this blog post**](https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html) for
an excellent description of the distributed locking problem.

In the first implementation of distributed locks, a thread would wait for a lock to be released by
polling the lock document every 500 milliseconds for 20 seconds (and return a LockBusy error if the
thread never saw the lock as available.) NamespaceSerializer locks were introduced to allow a thread
to be notified more efficiently when a lock held by another thread on the same node was released.
NamespaceSerializer locks were added only to DDL operations which were seen to frequently fail with
"LockBusy" when run concurrently on the same database, such as dropCollection.

Global ResourceMutexes are the most recent, and are taken to serialize modifying specific config
collections, such as config.shards, config.chunks, and config.tags. For example, splitChunk,
mergeChunks, and moveChunk all take the chunk ResourceMutex.

#### Code references
* [**DistLockManager class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/catalog/dist_lock_manager.h)
* [**DistLockCatalog class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/catalog/dist_lock_catalog.h)
* [**NamespaceSerializer class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/namespace_serializer.h)
* The interface for acquiring NamespaceSerializer locks
[**via the ShardingCatalogManager**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager.h#L276)
* The
[**global ResourceMutexes**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager.h#L555-L581)

---

# The logical clock and causal consistency

---

# Logical Sessions

Some operations, such as retryable writes and transactions, require durably storing metadata in the
cluster about the operation. However, it's important that this metadata does not remain in the
cluster forever.

Logical sessions provide a way to durably store metadata for the _latest_ operation in a sequence of
operations. The metadata is reaped if the cluster does not receive a new operation under the logical
session for a reasonably long time (the default is 30 minutes).

A logical session is identified by a globally unique id (UUID), called the `lsid`. An `lsid` can be
either generated by the driver (or mongo shell) or via the `startSession` server command. 

The order of operations in the logical session that need to durably store metadata is defined by an
integer counter, called the `txnNumber`. When the cluster receives a retryable write or transaction
with a higher `txnNumber` than the previous known `txnNumber`, the cluster overwrites the previous
metadata with the metadata for the new operation.

Operations sent with an `lsid` that do not need to durably store metadata simply bump the time at
which the session's metadata expires.

## The logical sessions cache

## The logical sessions catalog

## Retryable writes

## The historical routing table

## Transactions

---

# Sharding component hierarchy
