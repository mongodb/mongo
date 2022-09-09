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

The routing table cache is a read-only in-memory cache. This cache keeps track of the last-seen
routing information for databases and sharded collections in a sharded cluster. An independent
copy of the routing table cache exists on each router and shard.

Operations consult the routing table cache in order to route requests to shards that hold data for
a collection.

### How the routing table cache refreshes

The authoritative routing information exists persisted to disk on the config server. Certain node
types load information (refresh) directly from the config server. Other node types refresh from an
intermediary source. At any given time, the state of the in-memory cache is the result of the
latest refresh from that node’s source. As a result, the cache may or may not be up-to-date with
its corresponding source.

We define the refresh behavior based on node type below:

| Node Type                             | Information Source                                          | Additional Behavior  |
| ---------                             | ------------------                                          | ------------------   |
| Router                                | Config server                                               | N/A                  |
| Shard server acting as replica set primary   | Config server                                               | Persists refreshed information to disk. This information should always be consistent with the in-memory cache. The persisted information is replicated to all replica set secondaries. |
| Shard server acting as replica set secondary | On-disk information replicated from the replica set primary | N/A                  |

When referring to a refresh in this section, we imply that the given node’s routing table cache
will update its in-memory state with its corresponding source.

### When the routing table cache will refresh

The routing table cache is “lazy.” It does not refresh from its source unless necessary. The cache
will refresh in two scenarios:

1. A request sent to a shard returns an error indicating that the shard’s known routing information for that request doesn't match the sender's routing information.
2. The cache attempts to access information for a collection that has been locally marked as having out-of-date routing information (stale).

Operations that change a collection’s routing information (for example, a moveChunk command that
updates a chunk’s location) will mark the local node’s routing table cache as “stale” for affected
shards. Subsequent attempts to access routing information for affected shards will block on a
routing table cache refresh. Some operations, such as dropCollection, will affect all shards. In
this case, the entire collection will be marked as “stale.” Accordingly, subsequent attempts to
access any routing information for the collection will block on a routing table cache refresh.

### Types of refreshes

The routing table cache performs two types of refreshes for a database or collection.

1. A full refresh clears all cached information, and replaces the cache with the information that exists on the node’s source.
2. An incremental refresh only replaces modified routing information from the node’s source.

An incremental refresh occurs when the routing table cache already has a local notion of the
collection or database. A full refresh occurs when:

* The cache has no notion of a collection or database, or
* A collection or database has been marked as dropped by a shard or the local node’s routing information, or
* A collection has otherwise been marked as having a mismatched epoch.

### Operational Caveats

1. If the routing table cache receives an error in attempting to refresh, it will retry up to twice before giving up and returning stale information.
2. If the gEnableFinerGrainedCatalogCacheRefresh startup parameter is disabled, then all attempts to access routing information for a stale namespace will block on a routing table cache refresh, regardless if a particular targeted shard is marked as stale.

#### Code References

* [The CatalogCache (routing table cache) class](https://github.com/mongodb/mongo/blob/master/src/mongo/s/catalog_cache.h)
* [The CachedDatabaseInfo class](https://github.com/mongodb/mongo/blob/62d9485657717bf61fbb870cb3d09b52b1a614dd/src/mongo/s/catalog_cache.h#L61-L81)

Methods that will mark routing table cache information as stale (sharded collection).

* [invalidateShardOrEntireCollectionEntryForShardedCollection](https://github.com/mongodb/mongo/blob/62d9485657717bf61fbb870cb3d09b52b1a614dd/src/mongo/s/catalog_cache.h#L226-L236)
* [invalidateEntriesThatReferenceShard](https://github.com/mongodb/mongo/blob/62d9485657717bf61fbb870cb3d09b52b1a614dd/src/mongo/s/catalog_cache.h#L270-L274)
* [invalidateCollectionEntry_LINEARIZABLE](https://github.com/mongodb/mongo/blob/32fe49396dec58836033bca67ad1360b1a80f03c/src/mongo/s/catalog_cache.h#L211-L216)

Methods that will mark routing table cache information as stale (database).

* [onStaleDatabaseVersion](https://github.com/mongodb/mongo/blob/62d9485657717bf61fbb870cb3d09b52b1a614dd/src/mongo/s/catalog_cache.h#L197-L205)
* [purgeDatabase](https://github.com/mongodb/mongo/blob/62d9485657717bf61fbb870cb3d09b52b1a614dd/src/mongo/s/catalog_cache.h#L282-L286)

## Shard versioning and database versioning

In a sharded cluster, the placement of collections is determined by a versioning protocol. We use
this versioning protocol in tracking the location of both chunks for sharded collections and
databases for unsharded collections.

### Shard versioning

The shard versioning protocol tracks the placement of chunks for sharded collections.

Each chunk has a version called the "chunk version." A chunk version is represented as C<E, T, M, m> and consists of four elements:

1. The *E* epoch  - an object ID shared among all chunks for a collection that distinguishes a unique instance of the collection.
1. The *T* timestamp - a new unique identifier for a collection introduced in version 5.0. The difference between epoch and timestamp is that timestamps are comparable, allowing for distinguishing between two instances of a collection in which the epoch/timestamp do not match.
1. The *M* major version - an integer used to specify a change on the data placement (i.e. chunk migration).
1. The *m* minor version - An integer used to specify that a chunk has been resized (i.e. split or merged).

To completely define the shard versioning protocol, we introduce two extra terms - the "shard
version" and "collection version."

1. Shard version - For a sharded collection, this is the highest chunk version seen on a particular shard. The version of the *i* shard is represented as SV<sub>i</sub><E<sub>SV<sub>i</sub></sub>, T<sub>SV<sub>i</sub></sub>, M<sub>SV<sub>i</sub></sub>, m<sub>SV<sub>i</sub></sub>>.
1. Collection version - For a sharded collection, this is the highest chunk version seen across all shards. The collection version is represented as CV<E<sub>cv</sub>, T<sub>cv</sub>, M<sub>cv</sub>, m<sub>cv</sub>>.

### Database versioning

The database versioning protocol tracks the placement of databases for unsharded collections. The
"database version" indicates on which shard a database currently exists. A database version is represented as DBV<uuid, T, Mod> and consists of two elements:

1. The UUID - a unique identifier to distinguish different instances of the database. The UUID remains unchanged for the lifetime of the database, unless the database is dropped and recreated.
1. The *T* timestamp - a new unique identifier introduced in version 5.0. Unlike the UUID, the timestamp allows for ordering database versions in which the UUID/Timestamp do not match.
1. The last modified field - an integer incremented when the database changes its primary shard.

### Versioning updates

Nodes that track chunk/database versions “lazily” load versioning information. A router or shard
will only find out that its internally-stored versioning information is stale via receiving changed
version information from another node.

For each request sent from an origin node to a remote node, the origin node will attach its cached
version information for the corresponding chunk or database. There are two possible versioning
scenarios:

1. If the remote node detects a shard version mismatch, the remote node will return a message to the origin node stating as such. Whichever node that reports having an older version will attempt to refresh. The origin node will then retry the request.
1. If the remote node and the origin node have the same version, the request will proceed.

### Types of operations that will cause the versioning information to become stale

Before going through the table that explains which operations modify the versioning information and how, it is important to give a bit more information about the move chunk operation. When we move a chunk C from the *i* shard to the *j* shard, where *i* and *j* are different, we end up updating the shard version of both shards. For the recipient shard (i.e. *j* shard), the version of the migrated chunk defines its shard version. For the donor shard (i.e. *i* shard) what we do is look for another chunk of that collection on that shard and update its version. That chunk is called the control chunk and its version defines the *i* shard version. If there are no other chunks, the shard version is updated to SV<sub>i</sub><E<sub>cv</sub>, T<sub>cv</sub>, 0, 0>.

Operation Type                                      | Version Modification Behavior                                                                                |
--------------                                      | -----------------------------                                                                                |
Moving a chunk C <br> C<E, T, M, m>                    | C<E<sub>cv</sub>, T<sub>cv</sub>, M<sub>cv</sub> + 1, 0> <br> ControlChunk<E<sub>cv</sub>, T<sub>cv</sub>, M<sub>cv</sub> + 1, 1> if any      |
Splitting a chunk C into n pieces <br> C<E, T, M, m>   | C<sub>new 1</sub><E<sub>cv</sub>, T<sub>cv</sub>, M<sub>cv</sub>, m<sub>cv</sub> + 1> <br> ... <br> C<sub>new n</sub><E<sub>cv</sub>, T<sub>cv</sub>, M<sub>cv</sub>, m<sub>cv</sub> + n>                                                         |
Merging chunks C<sub>1</sub>, ..., C<sub>n</sub> <br> C<sub>1</sub><E<sub>1</sub>, T<sub>1</sub>, M<sub>1</sub>, m<sub>1</sub>> <br> ... <br> C<sub>n</sub><E<sub>n</sub>, T<sub>n</sub>, M<sub>n</sub>, m<sub>n</sub>> <br> | C<sub>new</sub><E<sub>cv</sub>, T<sub>cv</sub>, M<sub>cv</sub>, m<sub>cv</sub> + 1>    |
Dropping a collection                               | The dropped collection doesn't have a SV - all chunks are deleted                                              |
Refining a collection's shard key                   | C<sub>i</sub><E<sub>new</sub>, T<sub>now</sub>, M<sub>i</sub>, m<sub>i</sub>>  forall i in 1 <= i  <= #Chunks                 |
Changing the primary shard for a DB <br> DBV<uuid, T, Mod> | DBV<uuid, T, Mod + 1>                                                                                       |
Dropping a database                                 | The dropped DB doesn't have a DBV                                                                            |

### Special versioning conventions

Chunk versioning conventions

Convention Type                           | Epoch        | Timestamp        | Major Version | Minor Version |
---------------                           | -----        |--------------    | ------------- | ------------- |
First chunk for sharded collection        | ObjectId()   | current time     | 1             | 0             |
Collection is unsharded                   | ObjectId()   | Timestamp()      | 0             | 0             |
Collection was dropped                    | ObjectId()   | Timestamp()      | 0             | 0             |
Ignore the chunk version for this request | Max DateTime | Timestamp::max() | 0             | 0             |

Database version conventions

Convention Type | UUID   | Timestamp | Last Modified |
--------------- | ----   | --------- | ------------- |
New database    | UUID() | current time | 1          |
Config database | UUID() | current time | 0          |
Admin database  | UUID() | current time | 0          |

#### Code references

* [The chunk version class](https://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk_version.h)
* [The database version IDL](https://github.com/mongodb/mongo/blob/master/src/mongo/s/database_version.idl)
* [The database version class](https://github.com/mongodb/mongo/blob/master/src/mongo/s/database_version.h)
* [Where shard versions are stored in a routing table cache](https://github.com/mongodb/mongo/blob/1df41757d5d1e04c51eeeee786a17b005e025b93/src/mongo/s/catalog_cache.h#L499-L500)
* [Where database versions are stored in a routing table cache](https://github.com/mongodb/mongo/blob/1df41757d5d1e04c51eeeee786a17b005e025b93/src/mongo/s/catalog_cache.h#L497-L498)
* [Method used to attach the shard version to outbound requests](https://github.com/mongodb/mongo/blob/1df41757d5d1e04c51eeeee786a17b005e025b93/src/mongo/s/cluster_commands_helpers.h#L118-L121)
* [Where shard versions are parsed in the ServiceEntryPoint and put on the OperationShardingState](https://github.com/mongodb/mongo/blob/1df41757d5d1e04c51eeeee786a17b005e025b93/src/mongo/db/service_entry_point_common.cpp#L1136-L1150)
* [Where shard versions are stored in a shard's filtering cache](https://github.com/mongodb/mongo/blob/554ec671f7acb6a4df62664f80f68ec3a85bccac/src/mongo/db/s/collection_sharding_runtime.h#L249-L253)
* [The method that checks the equality of a shard version on a shard](https://github.com/mongodb/mongo/blob/554ec671f7acb6a4df62664f80f68ec3a85bccac/src/mongo/db/s/collection_sharding_state.h#L126-L131)
* [The method that checks the equality of a database version on a shard](https://github.com/mongodb/mongo/blob/554ec671f7acb6a4df62664f80f68ec3a85bccac/src/mongo/db/s/database_sharding_state.h#L98-L103)
* [Where stale config exceptions are handled on a shard](https://github.com/mongodb/mongo/blob/8fb7a62652c5fe54da47eab77e28111f00b99d7f/src/mongo/db/service_entry_point_mongod.cpp#L187-L213)
* [Where a mongos catches a StaleConfigInfo](https://github.com/mongodb/mongo/blob/5bd87925a006fa591692e097d7929b6764da6d0c/src/mongo/s/commands/strategy.cpp#L723-L780)
* [Where a cluster find catches a StaleConfigInfo](https://github.com/mongodb/mongo/blob/5bd87925a006fa591692e097d7929b6764da6d0c/src/mongo/s/query/cluster_find.cpp#L578-L585)

## The shard registry

The shard registry is an in-memory cache mirroring the `config.shards` collection on the config
server. The collection (and thus the cache) contains an entry for each shard in the cluster. Each
entry contains the connection string for that shard.

An independent cache exists on each node across all node types (router, shard server, config
server).

Retrieving a shard from the registry returns a `Shard` object. Using that object, one can access
more information about a shard and run commands against that shard. A `Shard` object can be
retrieved from the registry by using any of:

* The shard's name
* The replica set's name
* The HostAndPort object
* The connection string

The shard registry refreshes itself in these scenarios:

1. Upon the node's start-up
1. Upon completion of a background job that runs every thirty seconds
1. Upon an attempt to retrieve a shard that doesn’t have a matching entry in the cache
1. Upon calling the ShardRegistry’s reload function (ShardRegistry::reload())

The shard registry makes updates to the `config.shards` collection in one case. If the shard
registry discovers an updated connection string for another shard via a replica set topology
change, it will persist that update to `config.shards`.

#### Code references
* [The ShardRegistry class](https://github.com/mongodb/mongo/blob/master/src/mongo/s/client/shard_registry.h)
* [The Shard class](https://github.com/mongodb/mongo/blob/master/src/mongo/s/client/shard.h)

## Targeting a specific host within a shard
When routing a query to a replica set, a cluster node must determine which member to target for a given read preference. A cluster node either has or creates a ReplicaSetMonitor for each remote shard to which it needs to route requests. Information from the ReplicaSetMonitor interface is used to route requests to a specific node within a shard.

Further details on replica set monitoring and host targeting can be found [here](../../../mongo/client/README.md).

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
1. **Commit on the recipient** - While in the critical section, the [_recvChunkCommit](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/migration_chunk_cloner_source_legacy.cpp#L360) command is sent to the recipient directing it to fetch any remaining documents for this chunk. The recipient responds by sending _transferMods to fetch the remaining documents while writes are blocked on the donor. Once the documents are transferred successfully, the _recvChunkCommit command returns its status to unblock the donor.
1. **Commit on the config server** - The donor sends the _configsvrCommitChunkMigration command to the config server. Before the command is sent, [reads are also suspended](https://github.com/mongodb/mongo/blob/3f849d508692c038afb643b1acb99b8a8cb98d38/src/mongo/db/s/migration_source_manager.cpp#L436) on the donor shard.

#### Code references
* [ActiveMigrationRegistry](https://github.com/mongodb/mongo/blob/9be1041342b666e979aaea483c2fdb929c801796/src/mongo/db/s/active_migrations_registry.h#L52) class
* [MigrationSourceManager](https://github.com/mongodb/mongo/blob/2c87953010c2c1ec2d39dc9a7dbbd5f7d49dab10/src/mongo/db/s/migration_source_manager.h#L70) class
* [MigrationDestinationManager](https://github.com/mongodb/mongo/blob/2c87953010c2c1ec2d39dc9a7dbbd5f7d49dab10/src/mongo/db/s/migration_destination_manager.h#L71) class
* [MigrationChunkClonerSourceLegacy](https://github.com/mongodb/mongo/blob/11eddfac181ff6ff9faf3e1d55c050373bc6fc24/src/mongo/db/s/migration_chunk_cloner_source_legacy.h#L82) class
* [ShardingStateRecovery](https://github.com/mongodb/mongo/blob/2c87953010c2c1ec2d39dc9a7dbbd5f7d49dab10/src/mongo/db/s/sharding_state_recovery.h#L47) class

## Orphaned range deletion
After the migration protocol moves a chunk from one shard to another, the documents that were in the moved range need to be deleted from the donor shard. If the migration failed for any reason and was aborted, then any documents that have been copied over to the recipient need to be deleted. These documents are called orphans since they're not owned by the shard they reside on.

The migration protocol handles orphaned range deletion by recording the range that is being moved into the config.rangeDeletions collection on both the donor and recipient shards. The range deletion document contains the range that is to be deleted along with a pending flag that indicates if the range is ready for deletion.

If the migration completes successfully, the range is submitted for deletion on the donor and the range deletion document is deleted from the config.rangeDeletions collection on the recipient. If the migration fails, the range deletion document is deleted from the donor and the range is submitted for deletion on the recipient.

This sequence of steps is orchestrated by the MigrationCoordinator:
1. The donor shard writes the [migration coordinator document](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator_document.idl#L67-L102) to its local
config.migrationCoordinators collection. This document contains a unique ID along with other fields that are needed to recover the migration upon failure.
1. The donor shard writes the [range deletion document](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/range_deletion_task.idl#L50-L76) to its local config.rangeDeletions collection with the pending flag set to true. This will prevent the range from being deleted until it is marked as ready.
1. Before the recipient shard begins migrating documents from the donor, if there is an overlapping range already in the config.rangeDeletions collection, the recipient will [wait for it to be deleted](https://github.com/mongodb/mongo/blob/ea576519e5c3445bf11aa7f880aedbee1501010c/src/mongo/db/s/migration_destination_manager.cpp#L865-L885). The recipient then [writes a range deletion document](https://github.com/mongodb/mongo/blob/ea576519e5c3445bf11aa7f880aedbee1501010c/src/mongo/db/s/migration_destination_manager.cpp#L895) to its local config.rangeDeletions collection before the clone step begins.
1. Once the migration is completed, the MigrationCoordinator records the decision in the migration coordinator document on the donor.
    * If the migration succeeded, then the commit sequence is executed. This involves [deleting the range deletion document on the recipient](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator.cpp#L204) and then [marking the range](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator.cpp#L211) as ready to be deleted on the donor. The range is then [submitted for deletion](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator.cpp#L225) on the donor.
    * If the migration failed, then the abort sequence is executed. This involves [deleting the range deletion task on the donor](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator.cpp#L255) and then [marking the range as ready](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator.cpp#L261) to be deleted on the recipient. The range is then [submitted for deletion](https://github.com/mongodb/mongo/blob/52a73692175cad37f942ff5e6f3d70aacbbb113d/src/mongo/db/s/shard_server_op_observer.cpp#L383-L393) on the recipient by the ShardServerOpObserver when the [write is committed]((https://github.com/mongodb/mongo/blob/52a73692175cad37f942ff5e6f3d70aacbbb113d/src/mongo/db/s/shard_server_op_observer.cpp#L131)).
1. The migration coordinator document is then [deleted](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/migration_coordinator.cpp#L270).

On either donor or recipient, the range deletion is [submitted asynchronously](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/range_deletion_util.cpp#L396) to a separate executor that maintains one thread. On the donor, there is a risk of deleting documents that are being accessed in ongoing queries. We first wait for any queries on the primary to complete by [waiting on a promise](https://github.com/mongodb/mongo/blob/52a73692175cad37f942ff5e6f3d70aacbbb113d/src/mongo/db/s/metadata_manager.h#L212-L221) that is signalled by the [reference counting mechanism](https://github.com/mongodb/mongo/blob/ab21bf5ef46689cf4503a3b089def71113c437e2/src/mongo/db/s/metadata_manager.cpp#L126) in RangePreserver and [CollectionMetadataTracker](https://github.com/mongodb/mongo/blob/52a73692175cad37f942ff5e6f3d70aacbbb113d/src/mongo/db/s/metadata_manager.h#L201). We then [wait for a specified amount of time](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/range_deletion_util.cpp#L417-L418) for any queries running on secondaries to complete before starting the deletion. The delay defaults to 15 minutes but can be configured through a server parameter. The documents in the range are then [deleted in batches](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/range_deletion_util.cpp#L312) with a [delay between each batch](https://github.com/mongodb/mongo/blob/49159e1cf859d21c767f6b582dd6e6b2d675808d/src/mongo/db/s/range_deletion_util.cpp#L338). This rate limiting is intended to reduce the I/O load from excessive deletions happening at the same time. When the deletion has been completed, the [range deletion document is deleted](https://github.com/mongodb/mongo/blob/52a73692175cad37f942ff5e6f3d70aacbbb113d/src/mongo/db/s/range_deletion_util.cpp#L485) from the local config.rangeDeletions collection.

## Orphan filtering
There are two cases that arise where orphaned documents need to be filtered out from the results of commands. The first case occurs while the migration protocol described above is in progress. Queries on the recipient that include documents in the chunk that is being migrated will need to be filtered out. This is because this chunk is not yet owned by the recipient shard and should not be visible there until the migration commits.

The other case where orphans need to be filtered occurs once the migration is completed but the orphaned documents on the donor have not yet been deleted. The results of the filtering depend on what version of the chunk is in use by the query. If the query was in flight before the migration was completed, any documents that were moved by the migration must still be returned. The orphan deletion mechanism described above respects this and will not delete these orphans until the outstanding queries complete. If the query has started after the migration was committed, then the orphaned documents will not be returned since they are not owned by this shard.

Shards store a copy of the chunk distribution for each collection for which they own data. This copy, often called the "filtering metadata" since it is used to filter out orphaned documents for chunks the shard does not own, is stored in memory in the [CollectionShardingStateMap](https://github.com/mongodb/mongo/blob/r4.4.0-rc3/src/mongo/db/s/collection_sharding_state.cpp#L45). The map is keyed by namespace, and the values are instances of [CollectionShardingRuntime](https://github.com/mongodb/mongo/blob/8b8488340f53a71f29f40ead546e36c59323ca93/src/mongo/db/s/collection_sharding_runtime.h). A CollectionShardingRuntime stores the filtering metadata for the collection [in its MetadataManager member](https://github.com/mongodb/mongo/blob/8b8488340f53a71f29f40ead546e36c59323ca93/src/mongo/db/s/metadata_manager.h#L277-L281).

A query obtains a reference to the current filtering metadata for the collection
from the [MetadataManager](https://github.com/mongodb/mongo/blob/af62a3eeaf0b1101cb2f6e8e7595b70f2fe2f10f/src/mongo/db/s/metadata_manager.cpp#L162-L194) for the collection by calling the [CollectionShardingRuntime::getOwnershipFilter()](https://github.com/mongodb/mongo/blob/8b8488340f53a71f29f40ead546e36c59323ca93/src/mongo/db/s/collection_sharding_state.h#L99-L124) function. The MetadataManager keeps previous versions of the filtering metadata for queries that were still in flight before the migration was committed. If a cluster timestamp is specified, then an [earlier version](https://github.com/mongodb/mongo/blob/af62a3eeaf0b1101cb2f6e8e7595b70f2fe2f10f/src/mongo/db/s/metadata_manager.cpp#L177-L178) of the metadata is returned. The filtering metadata is [used by query commands](https://github.com/mongodb/mongo/blob/8b8488340f53a71f29f40ead546e36c59323ca93/src/mongo/db/query/stage_builder.cpp#L294-L305) to determine if a specific [document is owned](https://github.com/mongodb/mongo/blob/b9bd6ded04f0136157c50c85c8bdc6bb176cccc9/src/mongo/db/exec/shard_filter.cpp#L81) by the current shard.

## Replicating the orphan filtering table

---

# Auto-splitting and auto-balancing

Data may need to redistributed for many reasons, such as that a shard was added, a shard was
requested to be removed, or data was inserted in an imbalanced way.

The config server replica set durably stores settings for the maximum chunk size and whether chunks
should be automatically split and balanced.

## Auto-splitting
When the mongos routes an update or insert to a chunk, the chunk may grow beyond the configured
chunk size (specified by the server parameter maxChunkSizeBytes) and trigger an auto-split, which
partitions the oversized chunk into smaller chunks. The shard that houses the chunk is responsible
for:
* determining if the chunk should be auto-split
* selecting the split points
* committing the split points to the config server
* refreshing the routing table cache
* updating in memory chunk size estimates

### Deciding when to auto-split a chunk
The server schedules an auto-split if:
1. it detected that the chunk exceeds a threshold based on the maximum chunk size
2. there is not already a split in progress for the chunk

Every time an update or insert gets routed to a chunk, the server tracks the bytes written to the
chunk in memory through the collection's ChunkManager. The ChunkManager has a ChunkInfo object for
each of the collection's entries in the local config.chunks. Through the ChunkManager, the server
retrieves the chunk's ChunkInfo and uses its ChunkWritesTracker to increment the estimated chunk
size.

Even if the new size estimate exceeds the maximum chunk size, the server still needs to check that
there is not already a split in progress for the chunk. If the ChunkWritesTracker is locked, there
is already a split in progress on the chunk and trying to start another split is prohibited.
Otherwise, if the chunk is oversized and there is no split for the chunk in progress, the server
submits the chunk to the ChunkSplitter to be auto-split.

### The auto split task
The ChunkSplitter is a replica set primary-only service that manages the process of auto-splitting
chunks. The ChunkSplitter runs auto-split tasks asynchronously - thus, distinct chunks can
undergo an auto-split concurrently.

To prepare for the split point selection process, the ChunkSplitter flags that an auto-split for the
chunk is in progress. There may be incoming writes to the original chunk while the split is in
progress. For this reason, the estimated data size in the ChunkWritesTracker for this chunk is
reset, and the same counter is used to track the number of bytes written to the chunk while the
auto-split is in progress.

splitVector manages the bulk of the split point selection logic. First, the data size and number of
records are retrieved from the storage engine to approximate the number of keys that each chunk
partition should have. This number is calculated such that if each document were uniform in size,
each chunk would be half of maxChunkSizeBytes.

If the actual data size is less than the maximum chunk size, no splits are made at all.
Additionally, if all documents in the chunk have the same shard key, no splits can be made. In this
case, the chunk may be classified as a jumbo chunk.

In the general case, splitVector:
* performs an index scan on the shard key index
* adds every k'th key to the vector of split points, where k is the approximate number of keys each chunk should have
* returns the split points

If no split points were returned, then the auto-split task gets abandoned and the task is done.

If split points are successfully generated, the ChunkSplitter executes the final steps of the
auto-split task where the shard:
* commits the split points to config.chunks on the config server by removing the document containing
  the original chunk and inserting new documents corresponding to the new chunks indicated by the
split points
* refreshes the routing table cache
* replaces the original oversized chunk's ChunkInfo with a ChunkInfo object for each partition. The
  estimated data size for each new chunk is the number bytes written to the original chunk while the
auto-split was in progress

### Top Chunk Optimization
While there are several optimizations in the auto-split process that won't be covered here, it's
worthwhile to note the concept of top chunk optimization. If the chunk being split is the first or
last one on the collection, there is an assumption that the chunk is likely to see more insertions
if the user is inserting in ascending/descending order with respect to the shard key. So, in top
chunk optimization, the first (or last) key in the chunk is set as a split point. Once the split
points get committed to the config server, and the shard refreshes its CatalogCache, the
ChunkSplitter tries to move the top chunk out of the shard to prevent the hot spot from sitting on a
single shard.

#### Code references
* [**ChunkInfo**](https://github.com/mongodb/mongo/blob/18f88ce0680ab946760b599437977ffd60c49678/src/mongo/s/chunk.h#L44) class
* [**ChunkManager**](https://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk_manager.h) class
* [**ChunkSplitter**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/chunk_splitter.h) class
* [**ChunkWritesTracker**](https://github.com/mongodb/mongo/blob/master/src/mongo/s/chunk_writes_tracker.h) class
* [**splitVector**](https://github.com/mongodb/mongo/blob/18f88ce0680ab946760b599437977ffd60c49678/src/mongo/db/s/split_vector.cpp#L61) method
* [**splitChunk**](https://github.com/mongodb/mongo/blob/18f88ce0680ab946760b599437977ffd60c49678/src/mongo/db/s/split_chunk.cpp#L128) method
* [**commitSplitChunk**](https://github.com/mongodb/mongo/blob/18f88ce0680ab946760b599437977ffd60c49678/src/mongo/db/s/config/sharding_catalog_manager_chunk_operations.cpp#L316) method where chunk splits are committed

## Auto-balancing

The balancer is a background process that monitors the chunk distribution in a cluster. It is enabled by default and can be turned off for the entire cluster or per-collection at any time.

The balancer process [runs in a separate thread](https://github.com/mongodb/mongo/blob/b4094a6541bf5745cb225639c2486fcf390c4c38/src/mongo/db/s/balancer/balancer.cpp#L318-L490) on the config server primary. It runs continuously, but in "rounds" with a 10 second delay between each round. During a round, the balancer uses the current chunk distribution and zone information for the cluster to decide if any chunk migrations or chunk splits are necessary.

In order to retrieve the necessary distribution information, the balancer has a reference to the ClusterStatistics which is an interface that [obtains the data distribution and shard utilization statistics for the cluster](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/cluster_statistics_impl.cpp#L101-L166). During each round, the balancer uses the ClusterStatistics to get the current stats in order to [create a DistributionStatus for every collection](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/balancer_chunk_selection_policy_impl.cpp#L63-L116). The DistributionStatus contains information about which chunks are owned by which shards, the zones defined for the collection, and which chunks are a part of which zones. Note that because the DistributionStatus is per collection, this means that the balancer optimizes for an even distribution per collection rather than for the entire cluster.

### What happens during a balancer round

During each round, the balancer uses the DistributionStatus for each collection to [check if any chunk has a range that violates a zone boundary](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/balancer.cpp#L410). Any such chunks will be split into smaller chunks with new min and max values equal to the zone boundaries using the splitChunk command.

After any chunk splits have completed, the balancer then selects one or more chunks to migrate. The balancer again uses the DistributionStatus for each collection in order to select chunks to move. The balancer [prioritizes which chunks to move](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/balancer_policy.cpp#L360-L543) by the following:
1. If any chunk in this collection is owned by a shard that is draining (being removed), select this chunk first.
1. If no chunks for this collection belong to a draining shard, check for any chunks that violate zones.
1. If neither of the above is true, the balancer can select chunks to move in order to obtain the "ideal" number of chunks per shard. This value is calculated by dividing [the total number of chunks associated with some zone] / [the total number of shards associated with this zone]. For chunks that do not belong to any zone, this value is instead calculated by dividing [the total number of chunks that do not belong to any zone] / [the total number of shards]. The balancer will pick a chunk currently owned by the shard that is [most overloaded](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/balancer_policy.cpp#L272-L293) (has the highest number of chunks in the zone).

In each of these cases, the balancer will pick the ["least loaded" shard](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/balancer_policy.cpp#L244-L270) (the shard with the lowest number of chunks in the zone) as the recipient shard for the chunk. If a shard already has more chunks than the "ideal" number, it is draining, or it is already involved in a migration during this balancer round, the balancer will not pick this shard as the recipient. Similarly, the balancer will not select a chunk to move that is currently owned by a shard that is already involved in a migration. This is because a shard cannot be involved in more than one migration at any given time.

If the balancer has selected any chunks to move during a round, it will [schedule a migration for each of them](https://github.com/mongodb/mongo/blob/d501442a8ed07ba6e05cce3db8b83a5d7f4b7313/src/mongo/db/s/balancer/balancer.cpp#L631-L693) using the migration procedure outlined above. A balancer round is finished once all of the scheduled migrations have completed.

## Important caveats

### Jumbo Chunks

By default, a chunk is considered "too large to migrate" if its size exceeds the maximum size specified in the chunk size configuration parameter. If a chunk is this large and the balancer schedules either a migration or splitChunk, the migration or splitChunk will fail and the balancer will set the chunk's "jumbo" flag to true. However, if the balancer configuration setting 'attemptToBalanceJumboChunks' is set to true, the balancer will not fail a migration or splitChunk due to the chunk's size. Regardless of whether 'attemptToBalanceJumboChunks' is true or false, the balancer will not attempt to schedule a migration or splitChunk if the chunk's "jumbo" flag is set to true. Note that because a chunk's "jumbo" flag is not set to true until a migration or splitChunk has failed due to its size, it is possible for a chunk to be larger than the maximum chunk size and not actually be marked "jumbo" internally. The reason that the balancer will not schedule a migration for a chunk marked "jumbo" is to avoid the risk of forever scheduling the same migration or split - if a chunk is marked "jumbo" it means a migration or splitChunk has already failed. The clearJumboFlag command can be run for a chunk in order to clear its "jumbo" flag so that the balancer will schedule this migration in the future.

#### Code references
* [**Balancer class**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/balancer/balancer.h)
* [**BalancerPolicy class**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/balancer/balancer_policy.h)
* [**BalancerChunkSelectionPolicy class**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/balancer/balancer_chunk_selection_policy.h)
* [**ClusterStatistics class**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/balancer/cluster_statistics.h)

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
The business logic for most DDL commands that the config server coordinates lives in the
[**ShardingCatalogManager class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager.h#L86),
including the logic for
[**dropCollection**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager_collection_operations.cpp#L417).

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
network or NotPrimaryError. There are some cases where the sending node retries even though the
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
* [**DDLLockManager class**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/ddl_lock_manager.h)
* The
[**global ResourceMutexes**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/config/sharding_catalog_manager.h#L555-L581)

---

# The vector clock and causal consistency

The vector clock is used to manage various logical times and clocks across the distributed system, for the purpose of ensuring various guarantees about the ordering of distributed events (ie. "causality").

These causality guarantees are implemented by assigning certain _logical times_ to relevant _events_ in the system.  These logical times are strictly monotonically increasing, and are communicated ("gossiped") between nodes on all messages (both requests and responses).  This allows the order of distributed operations to be controlled in the same manner as with a Lamport clock.

## Vector clock

The VectorClock class manages these logical time, all of which have similar requirements (and possibly special relationships with each other).  There are currently three such components of the vector clock:

1. _ClusterTime_
1. _ConfigTime_
1. _TopologyTime_

Each of these has a type of LogicalTime, which is similar to Timestamp - it is an unsigned 64 bit integer representing a combination of unix epoch (high 32 bit) and an integer 32 bit counter (low 32 bit).  Together, the LogicalTimes for all vector clock components are known as the VectorTime.  Each LogicalTime must always strictly monotonically increase, and there are two ways that this can happen:

1. _Ticking_ is when a node encounters circumstances that require it to unilaterally increase the value.  This can either be some incremental amount (usually 1), or to some appropriate LogicalTime value.
1. _Advancing_ happens in response to learning about a larger value from some other node, ie. gossiping.

Each component has rules regarding when (and how) it is ticked, and when (and how) it is gossiped.  These define the system state that the component "tracks", what the component can be used for, and its relationship to the other components.

Since mongoses are stateless, they can never tick any vector clock component.  In order to enforce this, the VectorClockMutable class (a sub-class of VectorClock that provides the ticking API) is not linked on mongos.

## Component relationships

As explained in more detail below, certain relationships are preserved between between the vector clock components, most importantly:
```
ClusterTime >= ConfigTime >= TopologyTime
```

As a result, it is important to ensure that times are fetched correctly from the VectorClock.  The `getTime()` function returns a `VectorTime` which contains an atomic snapshot of all components.  Thus code should always be written such as:
```
auto currentTime = VectorClock::get(opCtx)->getTime();
doSomeWork(currentTime.clusterTime());
doOtherWork(currentTime.configTime());    // Always passes a timestamp <= what was passed to doSomeWork()
```

And generally speaking, code such as the following is incorrect:
```
doSomeWork(VectorClock::get(opCtx)->getTime().clusterTime());
doOtherWork(VectorClock::get(opCtx)->getTime().configTime());    // Might pass a timestamp > what was passed to doSomeWork()
```
because the timestamp received by `doOtherWork()` could be greater than the one received by `doSomeWork()` (ie. apparently violating the property).

To discourage this incorrect pattern, it is forbidden to use the result of getTime() as a temporary (r-value) in this way; it must always first be stored in a variable.

## ClusterTime

Starting from v3.6 MongoDB provides session based causal consistency. All operations in the causally
consistent session will be execute in the order that preserves the causality. In particular it
means that client of the session has guarantees to

* Read own writes
* Monotonic reads and writes
* Writes follow reads

Causal consistency guarantees described in details in the [**server
documentation**](https://docs.mongodb.com/v4.0/core/read-isolation-consistency-recency/#causal-consistency).

### ClusterTime ticking
The ClusterTime tracks the state of user databases.  As such, it is ticked only when the state of user databases change, i.e. when a mongod in PRIMARY state performs a write.  (In fact, there are a small number of other situations that tick ClusterTime, such as during step-up after a mongod node has won an election.)  The OpTime value used in the oplog entry for the write is obtained by converting this ticked ClusterTime to a Timestamp, and appending the current replication election term.

The ticking itself is performed by first incrementing the unix epoch part to the current walltime (if necessary), and then incrementing the counter by 1.  (Parallel insertion into the oplog will increment by N, rather than 1, and allocate the resulting range of ClusterTime values to the oplog entries.)

### ClusterTime gossiping
The ClusterTime is gossiped by all nodes in the system: mongoses, shard mongods, config server mongods, and clients such as drivers or the shell.  It is gossiped with both internal clients (other mongod/mongos nodes in the cluster) and external clients (drivers, the shell).  It uses the `$clusterTime` field to do this, using the `SignedComponentFormat` described below.

### ClusterTime example
Example of ClusterTime gossiping and ticking:
1. Client sends a write command to the primary, the message includes its current value of the ClusterTime: T1.
1. Primary node receives the message and advances its ClusterTime to T1, if T1 is greater than the primary
node's current ClusterTime value.
1. Primary node increments the cluster time to T2 in the process of preparing the OpTime for the write. This is
the only time a new value of ClusterTime is generated.
1. Primary node writes to the oplog.
1. Result is returned to the client, it includes the new ClusterTime T2.
1. The client advances its ClusterTime to T2.

### SignedComponentFormat: ClusterTime signing

As explained above, nodes advance their ClusterTime to the maximum value that they receive in the client
messages. The next oplog entry will use this value in the timestamp portion of the OpTime. But a malicious client
could modify their maximum ClusterTime sent in a message.  For example, it could send the `<greatest possible
cluster time - 1>`. This value, once written to the oplogs of replica set nodes, will not be incrementable (since LogicalTimes are unsigned) and the
nodes will then be unable to accept any changes (writes against the database). This ClusterTime
would eventually be gossiped across the entire cluster, affecting the availability of the whole
system.  The only ways to recover from this situation involve downtime (eg. dump and restore the
entire cluster).

To mitigate this risk, a HMAC-SHA1 signature is used to verify the value of the ClusterTime on the
server. ClusterTime values can be read by any node, but only MongoDB processes can sign new values.
The signature cannot be generated by clients. This means that servers can trust that validly signed
ClusterTime values supplied by (otherwise untrusted) clients must have been generated by a server.

Here is an example of the document that gossips ClusterTime:
```
"$clusterTime" : {
    "clusterTime" :
        Timestamp(1495470881, 5),
        "signature" : {
            "hash" : BinData(0, "7olYjQCLtnfORsI9IAhdsftESR4="),
            "keyId" : "6422998367101517844"
        }
}
```
The keyId is used to find the key that generated the hash.  The keys are stored and generated only on MongoDB
processes. This seals the ClusterTime value, as time can only be incremented on a server that has access to a signing key.
Every time the mongod or mongos receives a message that includes a
ClusterTime that is greater than the value of its logical clock, they will validate it by generating the signature using the key
with the keyId from the message. If the signature does not match, the message will be rejected.

### Key management
To provide HMAC message verification all nodes inside a security perimeter i.e. mongos and mongod  need to access a secret key to generate and
verify message signatures. MongoDB maintains keys in a `system.keys` collection in the `admin`
database. In a sharded cluster this collection is located on the config server replica set and managed by the config server primary.
In a replica set, this collection is managed by the primary node and propagated to secondaries via normal replication.

The key document has the following format:
```
{
    _id: <NumberLong>,
    purpose: <string>,
    key: <BinData>,
    expiresAt: <Timestamp>
}
```

The node that has the `system.keys` collection runs a thread that periodically checks if the keys
need to be updated, by checking its `expiresAt` field. The new keys are generated in advance, so
there is always one key that is valid for the next 3 months (the default). The signature validation
requests the key that was used for signing the message by its Id which is also stored in the
signature. Since the old keys are never deleted from the `system.keys` collection they are always
available to verify the messages signed in the past.

As the message verification is on the critical path each node also keeps an in memory cache of the
valid keys.

### Handling operator errors
The risk of malicious clients affecting ClusterTime is mitigated by a signature, but it is still possible to advance the
ClusterTime to the end of time by changing the wall clock value. This may happen as a result of operator error. Once
the data with the OpTime containing the end of time timestamp is committed to the majority of nodes it cannot be
changed. To mitigate this, there is a limit on the magnitude by which the (epoch part of the) ClusterTime can be
advanced.  This limit is the `maxAcceptableLogicalClockDriftSecs` parameter (default value is one year).

### Causal consistency in sessions
When a write event is sent from a client, that client has no idea what time is associated with the write, because the time
was assigned after the message was sent. But the node that processes the write does know, as it incremented its
ClusterTime and applied the write to the oplog. To make the client aware of the write's ClusterTime, it will be included
in the `operationTime` field of the response. To make sure that the client knows the time of all events, every
response (including errors) will include the `operationTime` field, representing the Stable Cluster
Time i.e. the ClusterTime of the latest item added to the oplog at the time the command was executed.
Now, to make the follow up read causally consistent the client will pass the exact time of the data it needs to read -
the received `operationTime` - in the `afterClusterTime` field of the request. The data node
needs to return data with an associated ClusterTime greater than or equal to the requested `afterClusterTime` value.

Below is an example of causally consistent "read own write" for the products collection that is sharded and has chunks on Shards A and B.
1. The client sends `db.products.insert({_id: 10, price: 100})` to a mongos and it gets routed to Shard A.
1. The primary on Shard A computes the ClusterTime, and ticks as described in the previous sections.
1. Shard A returns the result with the `operationTime` that was written to the oplog.
1. The client conditionally updates its local `lastOperationTime` value with the returned `operationTime` value
1. The client sends a read `db.products.aggregate([{$count: "numProducts"}])` to mongos and it gets routed to all shards where this collection has chunks: i.e. Shard A and Shard B.
  To be sure that it can "read own write" the client includes the `afterClusterTime` field in the request and passes the `operationTime` value it received from the write.
1. Shard B checks if the data with the requested OpTime is in its oplog. If not, it performs a noop write, then returns the result to mongos.
 It includes the `operationTime` that was the top of the oplog at the moment the read was performed.
1. Shard A checks if the data with the requested OpTime is in its oplog and returns the result to mongos. It includes the `operationTime` that was the top of the oplog at the moment the read was performed.
1. mongos aggregates the results and returns to the client with the largest `operationTime` it has seen in the responses from shards A and B.

## ConfigTime

ConfigTime is similar to the legacy `configOpTime` value used for causally consistent reads from config servers, but as a LogicalTime rather than an OpTime.

### ConfigTime ticking
The ConfigTime tracks the sharding state stored on the config servers.  As such, it is ticked only by config servers when they advance their majority commit point, and is ticked by increasing to that majority commit point value.  Since the majority commit point is based on oplog OpTimes, which are based the ClusterTime, this means that the ConfigTime ticks between ClusterTime values.  It also means that it is always true that ConfigTime <= ClusterTime, ie. ConfigTime "lags" ClusterTime.

The ConfigTime value is then used when querying the config servers to ensure that the returned state
is causally consistent.  This is done by using the ConfigTime as the parameter to `$afterOpTime`
field of the Read Concern (with an Uninitialised term, so that it's not used in comparisons), and as
the `minClusterTime` parameter to the read preference (to ensure that a current config server is
targeted, if possible).

### ConfigTime gossiping
The ConfigTime is gossiped only by sharded cluster nodes: mongoses, shard mongods, and config server mongods.  Clients (drivers/shell), and plain replica sets do not gossip ConfigTime.  In addition, ConfigTime is only gossiped with internal clients (other mongos/mongod nodes), as identified by the kInternalClient flag (set during the `hello` command sent by mongos/mongod).

It uses the `$configTime` field with the `PlainComponentFormat`, which simply represents the LogicalTime value as a Timestamp:
```
"$configTime" : Timestamp(1495470881, 5)
```

## TopologyTime

TopologyTime is related to the "topology" of the sharded cluster, in terms of the shards present.

### TopologyTime ticking
Since the TopologyTime tracks the cluster topology, it ticks when a shard is added or removed from the cluster.  This is done by ticking TopologyTime to the ConfigTime of the write issued by the `_configsvrAddShard` or `_configsvrRemoveShard` command.  Thus, the property holds that TopologyTime <= ConfigTime, ie. TopologyTime "lags" ConfigTime.

The TopologyTime value is then used by the ShardRegistry to know when it needs to refresh from the config servers.

### TopologyTime gossiping
The TopologyTime is gossiped identically to ConfigTime, except with a field name of `$topologyTime`.  (Note that this name is very similar to the unrelated `$topologyVersion` field returned by the streaming `hello` command response.)

## Code references

* [**Base VectorClock class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock.h) (contains querying, advancing, gossiping the time)
* [**VectorClockMutable class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock_mutable.h) (adds ticking and persistence, not linked on mongos)
* [**VectorClockMongoD class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock_mongod.cpp) (specific implementation used by mongod nodes)
* [**VectorClockMongoS class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/s/vector_clock_mongos.cpp) (specific implementation used by mongos nodes)

* [**Definition of which components use which gossiping format**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock.cpp#L322-L330)
* [**PlainComponentFormat class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock.cpp#L125-L155) (for gossiping without signatures, and persistence formatting)
* [**SignedComponentFormat class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock.cpp#L186-L320) (for signed gossiping of ClusterTime)
* [**LogicalTimeValidator class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/logical_time_validator.h) (generates and validates ClusterTime signatures)
* [**KeysCollectionManager class**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/keys_collection_manager.h) (maintains the ClusterTime signing keys in `admin.system.keys`)

* [**Definition of which components are gossiped internally/externally by mongod**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock_mongod.cpp#L389-L406)
* [**Definition of when components may be ticked by mongod**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/vector_clock_mongod.cpp#L408-L450)
* [**Definition of which components are gossiped internally/externally by mongos**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/s/vector_clock_mongos.cpp#L71-L79)

* [**Ticking ClusterTime**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/repl/local_oplog_info.cpp#L125) (main usage, search for `tickClusterTime` to find unusual cases)
* [**Ticking ConfigTime and TopologyTime**](https://github.com/mongodb/mongo/blob/3681b03baa/src/mongo/db/s/config_server_op_observer.cpp#L252-L256)

---

# Logical Sessions

Some operations, such as retryable writes and transactions, require durably storing metadata in the
cluster about the operation. However, it's important that this metadata does not remain in the
cluster forever.

Logical sessions provide a way to durably store metadata for the _latest_ operation in a sequence of
operations. The metadata is reaped if the cluster does not receive a new operation under the logical
session for a reasonably long time (the default is 30 minutes).

A logical session is identified by its "logical session id," or `lsid`. An `lsid` is a combination
of up to four pieces of information:

1. `id` - A globally unique id (UUID) generated by the mongo shell, driver, or the `startSession` server command
1. `uid` (user id) - The identification information for the logged-in user (if authentication is enabled)
1. `txnNumber` - An optional parameter set only for internal transactions spawned from retryable writes. Strictly-increasing counter set by the transaction API to match the txnNumber of the corresponding retryable write.
1. `txnUUID` - An optional parameter set only for internal transactions spawned inside client sessions. The txnUUID is a globally unique id generated by the transaction API.

A logical session with a `txnNumber` and `txnUUID` is considered a child of the session with matching `id` and `uid` values. There may be multiple child sessions per parent session, and checking out a child/parents session checks out the other and updates the `lastUsedTime` of both. Killing a parent session also kills all of its child sessions.

The order of operations in the logical session that need to durably store metadata is defined by an
integer counter, called the `txnNumber`. When the cluster receives a retryable write or transaction
with a higher `txnNumber` than the previous known `txnNumber`, the cluster overwrites the previous
metadata with the metadata for the new operation.

Operations sent with an `lsid` that do not need to durably store metadata simply bump the time at
which the session's metadata expires.

## The logical session cache

The logical session cache is an in-memory cache of sessions that are open and in use on a certain
node. Each node (router, shard, config server) has its own in-memory cache. A cache entry contains:
1. `_id` - The session’s logical session id
1. `user` - The session’s logged-in username (if authentication is enabled)
1. `lastUse` - The date and time that the session was last used

The in-memory cache periodically persists entries to the `config.system.sessions` collection, known
as the "sessions collection." The sessions collection has different placement behavior based on
whether the user is running a standalone node, a replica set, or a sharded cluster.

| Cluster Type    | Sessions Collection Durable Storage                                                                              |
|-----------------|------------------------------------------------------------------------------------------------------------------|
| Standalone Node | Sessions collection exists on the same node as the in-memory cache.                                              |
| Replica Set     | Sessions collection exists on the primary node and replicates to secondaries.                                    |
| Sharded Cluster | Sessions collection is a regular sharded collection - can exist on multiple shards and can have multiple chunks. |

### Session expiration

There is a TTL index on the `lastUse` field in the sessions collection. The TTL expiration date is
thirty (30) minutes out by default, but is user-configurable. This means that if no requests come
in that use a session for thirty minutes, the TTL index will remove the session from the sessions
collection. When the logical session cache performs its periodic refresh (defined below), it will
find all sessions that currently exist in the cache that no longer exist in the sessions
collection. This is the set of sessions that we consider “expired.” The expired sessions are then
removed from the in-memory cache.

### How a session gets placed into the logical session cache

When a node receives a request with attached session info, it will place that session into the
logical session cache. If a request corresponds to a session that already exists in the cache, the
cache will update the cache entry's `lastUse` field to the current date and time.

### How the logical session cache syncs with the sessions collection

At a regular interval of five (5) minutes (user-configurable), the logical session cache will sync
with the sessions collection. Inside the class, this is known as the "refresh" function. There are
four steps to this process:

1. All sessions that have been used on this node since the last refresh will be upserted to the sessions collection. This means that sessions that already exist in the sessions collection will just have their `lastUse` fields updated.
1. All sessions that have been ended in the cache on this node (via the endSessions command) will be removed from the sessions collection.
1. Sessions that have expired from the sessions collection will be removed from the logical session cache on this node.
1. All cursors registered on this node that match sessions that have been ended (step 2) or were expired (step 3) will be killed.

### Periodic cleanup of the session catalog and transactions table

The logical session cache class holds the periodic job to clean up the
[session catalog](#the-logical-session-catalog) and [transactions table](#the-transactions-table).
Inside the class, this is known as the "reap" function. Every five (5) minutes (user-configurable),
the following steps will be performed:

1. Find all sessions in the session catalog that were last checked out more than thirty minutes ago (default session expiration time).
1. For each session gathered in step 1, if the session no longer exists in the sessions collection (i.e. the session has expired or was explicitly ended), remove the session from the session catalog.
1. Find all entries in the transactions table that have a last-write date of more than thirty minutes ago (default session expiration time).
1. For each entry gathered in step 3, if the session no longer exists in the sessions collection (i.e. the session has expired or was explicitly ended), remove the entry from the transactions table.

#### Configurable parameters related to the logical session cache

| Parameter                                                                                                                                                          | Value Type | Default Value        | Startup/Runtime | Description                                                                                                                            |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------|----------------------|-----------------|----------------------------------------------------------------------------------------------------------------------------------------|
| [disableLogicalSessionCacheRefresh](https://github.com/mongodb/mongo/blob/9cbbb66d7536ab4f92baf99ef5332e96be0e4153/src/mongo/db/logical_session_cache.idl#L49-L54) | boolean    | false                | Startup         | Disables the logical session cache's periodic "refresh" and "reap" functions on this node. Recommended for testing only.               |
| [logicalSessionRefreshMillis](https://github.com/mongodb/mongo/blob/9cbbb66d7536ab4f92baf99ef5332e96be0e4153/src/mongo/db/logical_session_cache.idl#L34-L40)       | integer    | 300000ms (5 minutes) | Startup         | Changes how often the logical session cache runs its periodic "refresh" and "reap" functions on this node.                             |
| [localLogicalSessionTimeoutMinutes](https://github.com/mongodb/mongo/blob/9cbbb66d7536ab4f92baf99ef5332e96be0e4153/src/mongo/db/logical_session_id.idl#L191-L196)  | integer    | 30 minutes           | Startup         | Changes the TTL index timeout for the sessions collection. In sharded clusters, this parameter is supported only on the config server. |

#### Code references

* [Place where a session is placed (or replaced) in the logical session cache](https://github.com/mongodb/mongo/blob/1f94484d52064e12baedc7b586a8238d63560baf/src/mongo/db/logical_session_cache.h#L71-L75)
* [The logical session cache refresh function](https://github.com/mongodb/mongo/blob/1f94484d52064e12baedc7b586a8238d63560baf/src/mongo/db/logical_session_cache_impl.cpp#L207-L355)
* [The periodic job to clean up the session catalog and transactions table (the "reap" function)](https://github.com/mongodb/mongo/blob/1f94484d52064e12baedc7b586a8238d63560baf/src/mongo/db/logical_session_cache_impl.cpp#L141-L205)
* [Location of the session catalog and transactions table cleanup code on mongod](https://github.com/mongodb/mongo/blob/1f94484d52064e12baedc7b586a8238d63560baf/src/mongo/db/session/session_catalog_mongod.cpp#L331-L398)

## The logical session catalog

The logical session catalog of a mongod or mongos is an in-memory catalog that stores the runtime state
for sessions with transactions or retryable writes on that node. The runtime state of each session is
maintained by the session checkout mechanism, which also serves to serialize client operations on
the session. This mechanism requires every operation with an `lsid` and a `txnNumber` (i.e.
transaction and retryable write) to check out its session from the session catalog prior to execution,
and to check the session back in upon completion. When a session is checked out, it remains unavailable
until it is checked back in, forcing other operations to wait for the ongoing operation to complete
or yield the session.

Checking out an internal/child session additionally checks out its parent session (the session with the same `id` and `uid` value in the lsid, but without a `txnNumber` or `txnUUID` value), and vice versa.

The runtime state for a session consists of the last checkout time and operation, the number of operations
waiting to check out the session, and the number of kills requested. Retryable internal sessions are reaped from the logical session catalog [eagerly](https://github.com/mongodb/mongo/blob/67e37f8e806a6a5d402e20eee4b3097e2b11f820/src/mongo/db/session/session_catalog.cpp#L342), meaning that if a transaction session with a higher transaction number has successfully started, sessions with lower txnNumbers are removed from the session catalog and inserted into an in-memory buffer by the [InternalTransactionsReapService](https://github.com/mongodb/mongo/blob/67e37f8e806a6a5d402e20eee4b3097e2b11f820/src/mongo/db/internal_transactions_reap_service.h#L42) until a configurable threshold is met (1000 by default), after which they are deleted from the transactions table (`config.transactions`) and `config.image_collection` all at once. Eager reaping is best-effort, in that the in-memory buffer is cleared on stepdown or restart.

The last checkout time is used by
the [periodic job inside the logical session cache](#periodic-cleanup-of-the-session-catalog-and-transactions-table)
to determine when a session should be reaped from the session catalog, whereas the number of
operations waiting to check out a session is used to block reaping of sessions that are still in
use. The last checkout operation is used to determine the operation to kill when a session is
killed, whereas the number of kills requested is used to make sure that sessions are only killed on
the first kill request.

### The transactions table

The runtime state in a node's in-memory session catalog is made durable in the node's
`config.transactions` collection, also called its transactions table. The in-memory session catalog
 is
[invalidated](https://github.com/mongodb/mongo/blob/56655b06ac46825c5937ccca5947dc84ccbca69c/src/mongo/db/session/session_catalog_mongod.cpp#L324)
if the `config.transactions` collection is dropped and whenever there is a rollback. When
invalidation occurs, all active sessions are killed, and the in-memory transaction state is marked
as invalid to force it to be
[reloaded from storage the next time a session is checked out](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/session/session_catalog_mongod.cpp#L426).

#### Code references
* [**SessionCatalog class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/session/session_catalog.h)
* [**MongoDSessionCatalog class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/session/session_catalog_mongod.h)
* [**RouterSessionCatalog class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/session_catalog_router.h)
* How [**mongod**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/service_entry_point_common.cpp#L537) and [**mongos**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/commands/strategy.cpp#L412) check out a session prior to executing a command.

## Retryable writes

Retryable writes allow drivers to automatically retry non-idempotent write commands on network errors or failovers.
They are supported in logical sessions with `retryableWrites` enabled (default), with the caveat that the writes
are executed with write concern `w` greater than 0 and outside of transactions. [Here](https://github.com/mongodb/specifications/blob/49589d66d49517f10cc8e1e4b0badd61dbb1917e/source/retryable-writes/retryable-writes.rst#supported-write-operations)
is a complete list of retryable write commands.

When a command is executed as a retryable write, it is sent from the driver with `lsid` and `txnNumber` attached.
After that, all write operations inside the command are assigned a unique integer statement id `stmtId` by the
mongos or mongod that executes the command. In other words, each write operation inside a batch write command
is given its own `stmtId` and is individually retryable. The `lsid`, `txnNumber`, and `stmtId` constitute a
unique identifier for a retryable write operation.

This unique identifier enables a primary mongod to track and record its progress for a retryable
write command using the `config.transactions` collection and augmented oplog entries. The oplog
entry for a retryable write operation is written with a number of additional fields including
`lsid`, `txnNumber`, `stmtId` and `prevOpTime`, where `prevOpTime` is the opTime of the write that
precedes it. In certain cases, such as time-series inserts, a single oplog entry may encode
multiple client writes, and thus may contain an array value for `stmtId` rather than the more
typical single value. All of this results in a chain of write history that can be used to
reconstruct the result of writes that have already executed. After generating the oplog entry for a
retryable write operation, a primary mongod performs an upsert into `config.transactions` to write
a document containing the `lsid` (`_id`), `txnNumber`, `stmtId` and `lastWriteOpTime`, where
`lastWriteOpTime` is the opTime of the newly generated oplog entry. The `config.transactions`
collection is indexed by `_id` so this document is replaced every time there is a new retryable
write command (or transaction) on the session.

The opTimes for all committed statements for the latest retryable write command is cached in an [in-memory table](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/transaction_participant.h#L928) that gets [updated](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/transaction_participant.cpp#L2125-L2127) after each
write oplog entry is generated, and gets cleared every time a new retryable write command starts. Prior to executing
a retryable write operation, a primary mongod first checks to see if it has the commit opTime for the `stmtId` of
that write. If it does, the write operation is skipped and a response is constructed immediately based on the oplog
entry with that opTime. Otherwise, the write operation is performed with the additional bookkeeping as described above.
This in-memory cache of opTimes for committed statements is invalidated along with the entire in-memory transaction
state whenever the `config.transactions` is dropped and whenever there is rollback. The invalidated transaction
state is overwritten by the on-disk transaction history at the next session checkout.

To support retryability of writes across migrations, the session state for the migrated chunk is propagated
from the donor shard to the recipient shard. After entering the chunk cloning step, the recipient shard
repeatedly sends [\_getNextSessionMods](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/migration_chunk_cloner_source_legacy_commands.cpp#L240-L359) (also referred to as MigrateSession) commands to
the donor shard until the migration reaches the commit phase to clone any oplog entries that contain session
information for the migrated chunk. Upon receiving each response, the recipient shard writes the oplog entries
to disk and [updates](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/transaction_participant.cpp#L2142-L2144) its in-memory transaction state to restore the session state for the chunk.

### Retryable writes and findAndModify

For most writes, persisting only the (lsid, txnId) pair alone is sufficient to reconstruct a
response. For findAndModify however, we also need to respond with the document that would have
originally been returned. In version 5.0 and earlier, the default behavior is to
[record the document image into the oplog](https://github.com/mongodb/mongo/blob/33ad68c0dc4bda897a5647608049422ae784a15e/src/mongo/db/op_observer/op_observer_impl.cpp#L191)
as a no-op entry. The oplog entries generated would look something like:

* `{ op: "d", o: {_id: 1}, ts: Timestamp(100, 2), preImageOpTime: Timestamp(100, 1), lsid: ..., txnNumber: ...}`
* `{ op: "n", o: {_id: 1, imageBeforeDelete: "foobar"}, ts: Timestamp(100, 1)}`

There's a cost in "explicitly" replicating these images via the oplog. We've addressed this cost
with 5.1 where the default is to instead [save the image into a side collection](https://github.com/mongodb/mongo/blob/33ad68c0dc4bda897a5647608049422ae784a15e/src/mongo/db/op_observer/op_observer_impl.cpp#L646-L650)
with the namespace `config.image_collection`. A primary will add `needsRetryImage:
<preImage/postImage>` to the oplog entry to communicate to secondaries that they must make a
corollary write to `config.image_collection`.

Note that this feature was backported to 4.0, 4.2, 4.4 and 5.0. Released binaries with this
capability can be turned on by [setting the `storeFindAndModifyImagesInSideCollection` server
parameter](https://github.com/mongodb/mongo/blob/2ac9fd6e613332f02636c6a7ec7f6cff4a8d05ab/src/mongo/db/repl/repl_server_parameters.idl#L506-L512).

Partial cloning mechanisms such as chunk migrations, tenant migrations and resharding all support
the destination picking up the responsibility for satisfying a retryable write the source had
originally processed (to some degree). These cloning mechanisms naturally tail the oplog to pick up
on changes. Because the traditional retryable findAndModify algorithm places the images into the
oplog, the destination just needs to relink the timestamps for its oplog to support retryable
findAndModify.

For retry images saved in the image collection, the source will "downconvert" oplog entries with
`needsRetryImage: true` into two oplog entries, simulating the old format. As chunk migrations use
internal commands, [this downconverting procedure](https://github.com/mongodb/mongo/blob/0beb0cacfcaf7b24259207862e1d0d489e1c16f1/src/mongo/db/s/session_catalog_migration_source.cpp#L58-L97)
is installed under the hood. For resharding and tenant migrations, a new aggregation stage,
[_internalFindAndModifyImageLookup](https://github.com/10gen/mongo/blob/e27dfa10b994f6deff7c59a122b87771cdfa8aba/src/mongo/db/pipeline/document_source_find_and_modify_image_lookup.cpp#L61),
was introduced to perform the identical substitution. In order for this stage to have a valid timestamp
to assign to the forged no-op oplog entry as result of the "downconvert", we must always assign an
extra oplog slot when writing the original retryable findAndModify oplog entry with
`needsRetryImage: true`.

The server also supports [collection-level pre-images](https://docs.mongodb.com/realm/mongodb/trigger-preimages/#overview), a feature used by MongoDB Realm. This feature continues to store document pre-images in
the oplog, and is expected to work with the new retryable findAndModify behavior described above.
With this, it's possible that for a particular update operation, we must store a pre-image
(for collection-level pre-images) in the oplog while storing the post-image
(for retryable findAndModify) in `config.image_collection`. In order to avoid certain WiredTiger
constraints surrounding setting multiple timestamps in a single storage transaction, we must reserve
oplog slots before entering the OpObserver, which is where we would normally create an oplog entry
and assign it the next available timestamp. Here, we have a table that describes the different
scenarios, along with the timestamps that are reserved and the oplog entries assigned to each of
those timestamps:
| Parameters | NumSlotsReserved | TS - 2 | TS - 1 | TS | Oplog fields for entry with timestamp: TS |
| --- | --- | --- | --- | --- | --- |
| Update, NeedsRetryImage=postImage, preImageRecordingEnabled = True | 3 | No-op oplog entry storing the pre-image|Reserved for forged no-op entry eventually used by tenant migrations/resharding | Update oplog entry | NeedsRetryImage: postImage, preImageOpTime: \{TS - 2} |
| Update, NeedsRetryImage=preImage, preImageRecordingEnabled=True | 3 |No-op oplog entry storing the pre-image | Reserved but will not be used|Update Oplog entry | preImageOpTime: \{TS - 2} |
| Update, NeedsRetryImage=preImage, preImageRecordingEnabled=False | 2 | N/A | Reserved for forged no-op entry eventually used by tenant migrations/resharding|Update oplog entry|NeedsRetryImage: preImage |
| Update, NeedsRetryImage=postImage, preImageRecordingEnabled=False | 2 | N/A | Reserved for forged no-op entry eventually used by tenant migrations/resharding|Update oplog entry | NeedsRetryImage: postImage |
| Delete, NeedsRetryImage=preImage, preImageRecordingEnabled = True | 0. Note that the OpObserver will still create a no-op entry along with the delete oplog entry, assigning them the next two available timestamps (TS - 1 and TS respectively). | N/A | No-op oplog entry storing the pre-image|Delete oplog entry | preImageOpTime: \{TS - 1} |
|Delete, NeedsRetryImage=preImage, preImageRecordingEnabled = False|2|N/A|Reserved for forged no-op entry eventually used by tenant migrations/resharding|Delete oplog entry|NeedsRetryImage: preImage|

#### Code references
* [**TransactionParticipant class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/transaction_participant.h)
* How a write operation [checks if a statement has been executed](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/ops/write_ops_exec.cpp#L811-L816)
* How mongos [assigns statement ids to writes in a batch write command](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/write_ops/batch_write_op.cpp#L483-L486)
* How mongod [assigns statement ids to insert operations](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/ops/write_ops_exec.cpp#L573)
* [Retryable writes specifications](https://github.com/mongodb/specifications/blob/49589d66d49517f10cc8e1e4b0badd61dbb1917e/source/retryable-writes/retryable-writes.rst)

## Transactions

Cross-shard transactions provide ACID guarantees for multi-statement operations that involve documents on
multiple shards in a cluster. Similar to [transactions on a single replica set](https://github.com/mongodb/mongo/blob/r4.4.0-rc7/src/mongo/db/repl/README.md#transactions), cross-shard transactions are only supported in logical
sessions. They have a configurable lifetime limit, and are automatically aborted when they are expired
or when the session is killed.

To run a cross-shard transaction, a client sends all statements, including the `commitTransaction` and
`abortTransaction` command, to a single mongos with common `lsid` and `txnNumber` attached. The first
statement is sent with `startTransaction: true` to indicate the start of a transaction. Once a transaction
is started, it remains active until it is explicitly committed or aborted by the client, or unilaterally
aborted by a participant shard, or overwritten by a transaction with a higher `txnNumber`.

When a mongos executes a transaction, it is responsible for keeping track of all participant shards, and
choosing a coordinator shard and a recovery shard for the transaction. In addition, if the transaction
uses read concern `"snapshot"`, the mongos is also responsible for choosing a global read timestamp (i.e.
`atClusterTime`) at the start of the transaction. The mongos will, by design, always choose the first participant
shard as the coordinator shard, and the first shard that the transaction writes to as the recovery shard.
Similarly, the global read timestamp will always be the logical clock time on the mongos when it receives
the first statement for the transaction. If a participant shard cannot provide a snapshot at the chosen
read timestamp, it will throw a snapshot error, which will trigger a client level retry of the transaction.
The mongos will only keep this information in memory as it relies on the participant shards to persist their
respective transaction states in their local `config.transactions` collection.

The execution of a statement inside a cross-shard transaction works very similarly to that of a statement
outside a transaction. One difference is that mongos attaches the transaction information (e.g. `lsid`,
`txnNumber` and `coordinator`) in every statement it forwards to targeted shards. Additionally, the first
statement to a participant shard is sent with `startTransaction: true` and `readConcern`, which contains
the `atClusterTime` if the transaction uses read concern `"snapshot"`. When a participant shard receives
a transaction statement with `coordinator: true` for the first time, it will infer that it has been chosen
as the transaction coordinator and will set up in-memory state immediately to prepare for coordinating
transaction commit. One other difference is that the response from each participant shard includes an
additional `readOnly` flag which is set to true if the statement does not do a write on the shard. Mongos
uses this to determine how a transaction should be committed or aborted, and to choose the recovery shard
as described above. The id of the recovery shard is included in the `recoveryToken` in the response to
the client.

### Committing a Transaction

The commit procedure begins when a client sends a `commitTransaction` command to the mongos that the
transaction runs on. The command is retryable as long as no new transaction has been started on the session
and the session is still alive. The number of participant shards and the number of write shards determine
the commit path for the transaction.

* If the number of participant shards is zero, the mongos skips the commit and returns immediately.
* If the number of participant shards is one, the mongos forwards `commitTransaction` directly to that shard.
* If the number of participant shards is greater than one:
   * If the number of write shards is zero, the mongos forwards `commitTransaction` to each shard individually.
   * Otherwise, the mongos sends `coordinateCommitTransaction` with the participant list to the coordinator shard to
   initiate two-phase commit.

To recover the commit decision after the original mongos has become unreachable, the client can send `commitTransaction`
along with the `recoveryToken` to a different mongos. This will not initiate committing the transaction, instead
the mongos will send `coordinateCommitTransaction` with an empty participant list to the recovery shard to try to
join the progress of the existing coordinator if any, and to retrieve the commit outcome for the transaction.

#### Two-phase Commit Protocol

The two-phase commit protocol consists of the prepare phase and the commit phase. To support recovery from
failovers, a coordinator keeps a document inside the `config.transaction_coordinators` collection that contains
information about the transaction it is trying commit. This document is deleted when the commit procedure finishes.

Below are the steps in the two-phase commit protocol.

* Prepare Phase
  1. The coordinator writes the participant list to the `config.transaction_coordinators` document for the
transaction, and waits for it to be majority committed.
  1. The coordinator sends [`prepareTransaction`](https://github.com/mongodb/mongo/blob/r4.4.0-rc7/src/mongo/db/repl/README.md#lifetime-of-a-prepared-transaction) to the participants, and waits for vote reponses. Each participant
shard responds with a vote, marks the transaction as prepared, and updates the `config.transactions`
document for the transaction.
  1. The coordinator writes the decision to the `config.transaction_coordinators` document and waits for it to
be majority committed. If the `coordinateCommitTransactionReturnImmediatelyAfterPersistingDecision` server parameter is
true  (default), the  `coordinateCommitTransaction` command returns immediately after waiting for client's write concern
(i.e. let the remaining work continue in the background).

* Commit Phase
  1. If the decision is 'commit', the coordinator sends `commitTransaction` to the participant shards, and waits
for responses. If the decision is 'abort', it sends `abortTransaction` instead. Each participant shard marks
the transaction as committed or aborted, and updates the `config.transactions` document.
  1. The coordinator deletes the coordinator document with write concern `{w: 1}`.

The prepare phase is skipped if the coordinator already has the participant list and the commit decision persisted.
This can be the case if the coordinator was created as part of step-up recovery.

### Aborting a Transaction

Mongos will implicitly abort a transaction on any error except the view resolution error from a participant shard
if a two phase commit has not been initiated. To explicitly abort a transaction, a client must send an `abortTransaction`
command to the mongos that the transaction runs on. The command is also retryable as long as no new transaction has
been started on the session and the session is still alive. In both cases, the mongos simply sends `abortTransaction`
to all participant shards.

#### Code references
* [**TransactionRouter class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/s/transaction_router.h)
* [**TransactionCoordinatorService class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/transaction_coordinator_service.h)
* [**TransactionCoordinator class**](https://github.com/mongodb/mongo/blob/r4.3.4/src/mongo/db/s/transaction_coordinator.h)

## Internal Transactions

Internal transactions are transactions that mongos and mongod can run on behalf of a client command regardless of a client's session option configuration. These transactions are started and managed internally by mongos/mongod, thus clients are unaware of the execution of internal transactions. All internal transactions will be run within an a session started internally, which we will refer to as `internal sessions`, except for in the case where the client is already running a transaction within a session, to which we let the transaction execute as a regular client transaction.

An internal transaction started on behalf of a client command is subject to the client command's constraints such as terminating execution if the command's `$maxTimeMS` is reached, or guaranteeing retryability if the issued command was a retryable write. These constraints lead to the following concepts.

### Non-Retryable Internal Transactions

If a client runs a command in a without a session or with session where retryable writes are disabled I.E. `retryWrites: false`, the server will start a non-retryable internal transaction.

### Retryable Internal Transactions

If a client runs a command in a session where retryable writes are enabled I.E. `retryWrites: true`, the server will start a retryable internal transaction.

**Note**: The distinction between **Retryable** and **Non-Retryable** here is the requirement that Retryable Internal Transactions must fulfill the retryable write contract, which is described below. Both types of transactions will be [retried internally on transient errors](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_api.cpp#L201-L221). The only exception is an internal transaction that is started on behalf of a `client transaction`, which can only be retried by the client.

#### How retryability is guaranteed

We expect that retryable write commands that start retryable internal transactions conform to the retryable write contract which has the following stipulations:

1. Write statements within the command are guaranteed to apply only once regardless of how many times a client retries.
2. The response for the command is guaranteed to be reconstructable on retry.

To do this, retryable write statements executed inside of a retryable internal transaction try to emulate the behavior of ordinary retryable writes. 

Each statement inside of a retryable write command should have a corresponding entry within a retryable internal transaction with the same `stmtId` as the original write statement. When a transaction participant for a retryable internal transaction notices a write statement with a previously seen `stmtId`, it will not execute the statement and instead generate the original response for the already executed statement using the oplog entry generated by the initial execution. The check for previously executed statements is done using the `retriedStmtIds` array, which contains the `stmtIds` of already retried statements, inside of a write command's response. 

In cases where a client retryable write command implicitly expects an auxiliary operation to be executed atomically with its current request, a retryable internal transaction may contain additional write statements that are not explicitly requested by a client retryable write command. An example could be that the client expects to atomically update an index when executing a write. Since these auxiliary write statements do not have a corresponding entry within the original client command, the `stmtId` field for these statements will be set to `{stmtId: kUninitializedStmtId}`. These auxiliary write statements are non-retryable, thus it is crucial that we use the `retriedStmtIds` to determine which client write statements were already successfully retried to avoid re-applying the corresponding auxilary write statements. Additionally, these statements will be excluded from the history check involving `retriedStmtIds`. 

To guarantee that we can reconstruct the response regardless of retries, we do a "cross sectional" write history check for retryable writes and retryable internal transactions prior to running a client retryable write/retryable internal transaction command. This ensures we do not double apply non-idempotent operations, and instead recover the response for a successful execution when appropriate. To support this, the [RetryableWriteTransactionParticipantCatalog](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.h#L1221-L1299) was added as a decoration on an external session and it stores the transaction participants for all active retryable writes on the session, which we use to do our [write history check](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L3206).

#### Reconstructing write responses

To reconstruct responses for retryable internal transactions, we use the applyOps oplog entry, which contains an inner entry with the operation run under the `o` field that has a corresponding `stmtId`. We use the `stmtId` and `opTime` cached in the `TransactionParticipant` to lookup the operation in the applyOps oplog entry, which gives us the necessary details to reconstruct the original write response. The process for reconstructing retryable write responses works the same way.


#### Special considerations for findAndModify

`findAndModify` additionally, requires the storage of pre/post images. The behavior of recovery differs based on the setting of `storeFindAndModifyImagesInSideCollection`.

If `storeFindAndModifyImagesInSideCollection` is **false**, then upon committing or preparing an internal transaction, we generate a no-op oplog entry that stores either stores the pre or post image of the document involved. The operation entry for the `findAndModify` statement inside the applyOps oplog entry will have a `preImageOpTime` or a `postImageOpTime` field that is set to the opTime of the no-op oplog entry. That opTime will be used to lookup the pre/post image when reconstructing the write response.

If `storeFindAndModifyImagesInSideCollection` is **true**, then upon committing or preparing an internal transaction, we insert a document into `config.image_collection` containing the pre/post image. The operation entry for the findAndModify statement inside the applyOps oplog entry will have a `needsRetryImage` field that is set to `true` to indicate that a pre/post image should be loaded from the side collection when reconstructing the write response. We can do the lookup using a transaction's `lsid` and `txnNumber`.

Currently, a retryable internal transaction can only support a **single** `findAndModify` statement at a time, due to the limitation that `config.image_collection` can only support storing one pre/post image entry for a given `(lsid, txnNumber)`. 

#### Retryability across failover and restart

To be able to guarantee retryability under failover, we need to make sure that a mongod **always** has all the necessary transaction state loaded while executing a retryable write command. To do this, we recover the transaction state of the client and internal sessions [when checking out sessions](https://github.com/mongodb/mongo/blob/master/src/mongo/db/session/session_catalog_mongod.cpp#L694) on recovery. During checkout, we call [refreshFromStorageIfNeeded()](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L2902) on the current client session (if we are running in one) to refresh the TransactionParticipant for that session. We then [fetch any relevant active internal sessions associated with the current client session and refresh the TransactionParticipants for those sessions](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L2988).

#### Handling retry conflicts

Due to the use of `txnUUID` in the lsid for de-duplication purposes, retries of client write statements will always spawn a different internal session/transaction than the one originally used to do the initial attempt. This has two implications for conflict resolution:

1. If the client retries on the same mongos/mongod that the original write was run on, retries are blocked by mongos/mongod until the original attempt finishes execution. This is due to the [session checkout mechanism](https://github.com/mongodb/mongo/blob/master/src/mongo/db/service_entry_point_common.cpp#L941) that prevents checkout of an in-use session, which in this case would block the retry attempt from checking out the parent session. Once the original write finishes execution, the retry would either retry(if necessary) or recover the write response as described above.

2. If the client retries on a different mongos than the original write was run on, the new mongos will not have visibility over in-progress internal transactions run by another mongos, so this retry will not be blocked and legally begin execution. When the new mongos begins execution of the retried command, it will send commands with `startTransaction` to relevant transaction participants. The transaction participants will then [check if there is already an in-progress internal transaction that will conflict](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L2814-L2868) with the new internal transaction that is attempting to start. If so, then the transaction participant will throw `RetryableTransactionInProgress`, which will be caught and cause the new transaction to [block until the existing transaction is finished](https://github.com/mongodb/mongo/blob/master/src/mongo/db/service_entry_point_common.cpp#L1001-L1010).


#### Supporting retryability across chunk migration and resharding

 The session history, oplog entries, and image collection entries involving the chunk being migrated are cloned from the donor shard to the recipient shard during chunk migration. Once the recipient receives the relevant oplog entries from the donor, it will [nest and apply the each of the received oplog entries in a no-op oplog entry](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/session_catalog_migration_destination.cpp#L204-L347). Depending on the type of operation run, the behavior will differ as such. 

* If a non-retryable write/non-retryable internal transaction is run, then the donor shard will [send a sentinel no-op oplog entry](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/session_catalog_migration_source.cpp#L657), which when parsed by the TransactionParticipant upon getting a retry against the recipient shard will [throw IncompleteTransactionHistory](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L328-L333). 

* If a retryable write/retryable internal transaction is run, then the donor shard will send a ["downconverted" oplog entry](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/session_catalog_migration_source.cpp#L673-L684), which when parsed by the TransactionParticipant upon getting a retry against the recipient shard will return the original write response.

`Note`: "Downconverting" in this context, is the process of extracting the operation information inside an applyOps entry for an internal transaction and constructing a new retryable write oplog entry with `lsid` and `txnNumber` set to the associated client's session id and txnNumber. 

For resharding, the process is similar to how chunk migrations are handled. The session history, oplog entries, and image collection entries for operations run during resharding are cloned from the donor shard to the recipient shard. The only difference is that the recipient in this case will handle the "downconverting", nesting, and applying of the received oplog entries. The two cases discussed above apply to resharding as well.


#### Code References

* [**Session checkout logic**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/session/session_catalog_mongod.cpp#L694)
* [**Cross-section history check logic**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L3206)
* [**Conflicting internal transaction check logic**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L2814-L2868)
* [**Refreshing client and internal sessions logic**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.cpp#L2923-L2986)
* [**RetryableWriteTransactionParticipantCatalog**](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction_participant.h#L1221-L1299)

### Transaction API

The [transaction API](https://github.com/mongodb/mongo/blob/master/src/mongo/db/transaction/transaction_api.h) is used to initiate transactions from within the server. The API starts an internal transaction on its local process, executes transaction statements specified in a callback, and completes the transaction by committing/aborting/retrying on transient errors. By default, a transaction can be retried 120 times to mirror the 2 minute timeout used by the [driver’s convenient transactions API](https://github.com/mongodb/specifications/blob/92d77a6d/source/transactions-convenient-api/transactions-convenient-api.rst).

Additionally, the API can use router commands when running on a mongod. Each command will execute as if on a mongos, targeting remote shards and initiating a two phase commit if necessary. To enable this router behavior the [`cluster_transaction_api`](https://github.com/mongodb/mongo/blob/master/src/mongo/db/cluster_transaction_api.h) defines an additional set of behaviors to rename commands to their [cluster command names](https://github.com/mongodb/mongo/blob/63f99193df82777239f038666270e4bfb2be3567/src/mongo/db/cluster_transaction_api.cpp#L44-L52).

Transactions for non-retryable operations or operations without a session initiated through the API use sessions from the [InternalSessionPool](https://github.com/mongodb/mongo/blob/master/src/mongo/db/internal_session_pool.h) to prevent the creation and maintenance of many single-use sessions.

To use the transaction API, [instantiate a transaction client](https://github.com/mongodb/mongo/blob/63f99193df82777239f038666270e4bfb2be3567/src/mongo/s/commands/cluster_find_and_modify_cmd.cpp#L250-L253) by providing the opCtx, an executor, and resource yielder. Then, run the commands to be grouped in the same transaction session on the transaction object. Some examples of this are listed below. 

* [Cluster Find and Modify Command](https://github.com/mongodb/mongo/blob/63f99193df82777239f038666270e4bfb2be3567/src/mongo/s/commands/cluster_find_and_modify_cmd.cpp#L255-L265)
* [Queryable Encryption](https://github.com/mongodb/mongo/blob/63f99193df82777239f038666270e4bfb2be3567/src/mongo/db/commands/fle2_compact.cpp#L636-L648)
* [Cluster Write Command - WouldChangeOwningShard Error](https://github.com/mongodb/mongo/blob/63f99193df82777239f038666270e4bfb2be3567/src/mongo/s/commands/cluster_write_cmd.cpp#L162-L190)

## The historical routing table

When a mongos or mongod executes a command that requires shard targeting, it must use routing information
that matches the read concern of the command. If the command uses `"snapshot"` read concern, it must use
the historical routing table at the selected read timestamp. If the command uses any other read concern,
it must use the latest cached routing table.

The [routing table cache](#the-routing-table-cache) provides an interface for obtaining the routing table
at a particular timestamp and collection version, namely the `ChunkManager`. The `ChunkManager` has an
optional clusterTime associated with it and a `RoutingTableHistory` that contains historical routing
information for all chunks in the collection. That information is stored in an ordered map from the max
key of each chunk to an entry that contains routing information for the chunk, such as chunk range,
chunk version and chunk history. The chunk history contains the shard id for the shard that currently
owns the chunk, and the shard id for any other shards that used to own the chunk in the past
`minSnapshotHistoryWindowInSeconds` (defaults to 300 seconds). It corresponds to the chunk history in
the `config.chunks` document for the chunk which gets updated whenever the chunk goes through an
operation, such as merge or migration. The `ChunkManager` uses this information to determine the
shards to target for a query. If the clusterTime is not provided, it will return the shards that
currently own the target chunks. Otherwise, it will return the shards that owned the target chunks
at that clusterTime and will throw a `StaleChunkHistory` error if it cannot find them.

#### Code references
* [**ChunkManager class**](https://github.com/mongodb/mongo/blob/r4.3.6/src/mongo/s/chunk_manager.h#L233-L451)
* [**RoutingTableHistory class**](https://github.com/mongodb/mongo/blob/r4.3.6/src/mongo/s/chunk_manager.h#L70-L231)
* [**ChunkHistory class**](https://github.com/mongodb/mongo/blob/r4.3.6/src/mongo/s/catalog/type_chunk.h#L131-L145)

---

# Node startup and shutdown

## Startup and sharding component initialization
The mongod intialization process is split into three phases. The first phase runs on startup and initializes the set of stateless components based on the cluster role. The second phase then initializes additional components that must be initialized with state read from the config server. The third phase is run on the [transition to primary](https://github.com/mongodb/mongo/blob/879d50a73179d0dd94fead476468af3ee4511b8f/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L822-L901) and starts services that only run on primaries.

### Shard Server initialization

#### Phase 1:
1. On a shard server, the `CollectionShardingState` factory is set to an instance of the `CollectionShardingStateFactoryShard` implementation. The component lives on the service context.
1. The sharding [OpObservers are created](https://github.com/mongodb/mongo/blob/0e08b33037f30094e9e213eacfe16fe88b52ff84/src/mongo/db/mongod_main.cpp#L1000-L1001) and registered with the service context. The `OpObserverShardingImpl` class forwards operations during migration to the chunk cloner. The `ShardServerOpObserver` class is used to handle the majority of sharding related events. These include loading the shard identity document when it is inserted and performing range deletions when they are marked as ready.

#### Phase 2:
1. The [shardIdentity document is loaded](https://github.com/mongodb/mongo/blob/37ff80f6234137fd314d00e2cd1ff77cde90ce11/src/mongo/db/s/sharding_initialization_mongod.cpp#L366-L373) if it already exists on startup. For shards, the shard identity document specifies the config server connection string. If the shard does not have a shardIdentity document, it has not been added to a cluster yet, and the "Phase 2" initialization happens when the shard receives a shardIdentity document as part of addShard.
1. If the shard identity document was found, then the [ShardingState is intialized](https://github.com/mongodb/mongo/blob/37ff80f6234137fd314d00e2cd1ff77cde90ce11/src/mongo/db/s/sharding_initialization_mongod.cpp#L416-L462) from its fields.
1. The global sharding state is set on the Grid. The Grid contains the sharding context for a running server. It exists both on mongod and mongos because the Grid holds all the components needed for routing, and both mongos and shard servers can act as routers.
1. `KeysCollectionManager` is set on the `LogicalTimeValidator`.
1. The `ShardingReplicaSetChangeListener` is instantiated and set on the `ReplicaSetMonitor`.
1. The remaining sharding components are [initialized for the current replica set role](https://github.com/mongodb/mongo/blob/37ff80f6234137fd314d00e2cd1ff77cde90ce11/src/mongo/db/s/sharding_initialization_mongod.cpp#L255-L286) before the Grid is marked as initialized.

#### Phase 3:
Shard servers [start up several services](https://github.com/mongodb/mongo/blob/879d50a73179d0dd94fead476468af3ee4511b8f/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L885-L894) that only run on primaries.

### Config Server initialization

#### Phase 1:
The sharding [OpObservers are created](https://github.com/mongodb/mongo/blob/0e08b33037f30094e9e213eacfe16fe88b52ff84/src/mongo/db/mongod_main.cpp#L1000-L1001) and registered with the service context. The config server registers the OpObserverImpl and ConfigServerOpObserver observers.

#### Phase 2:
The global sharding state is set on the Grid. The Grid contains the sharding context for a running server. The config server does not need to be provided with the config server connection string explicitly as it is part of its local state.

#### Phase 3:
Config servers [run some services](https://github.com/mongodb/mongo/blob/879d50a73179d0dd94fead476468af3ee4511b8f/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L866-L867) that only run on primaries.

### Mongos initialization
#### Phase 2:
The global sharding state is set on the Grid. The Grid contains the sharding context for a running server. Mongos is provided with the config server connection string as a startup parameter.

#### Code references
* Function to [initialize global sharding state](https://github.com/mongodb/mongo/blob/eeca550092d9601d433e04c3aa71b8e1ff9795f7/src/mongo/s/sharding_initialization.cpp#L188-L237).
* Function to [initialize sharding environment](https://github.com/mongodb/mongo/blob/37ff80f6234137fd314d00e2cd1ff77cde90ce11/src/mongo/db/s/sharding_initialization_mongod.cpp#L255-L286) on shard server.
* Hook for sharding [transition to primary](https://github.com/mongodb/mongo/blob/879d50a73179d0dd94fead476468af3ee4511b8f/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L822-L901).

## Shutdown

If the mongod server is primary, it will [try to step down](https://github.com/mongodb/mongo/blob/0987c120f552ab6d347f6b1b6574345e8c938c32/src/mongo/db/mongod_main.cpp#L1046-L1072). Mongod and mongos then run their respective shutdown tasks which cleanup the remaining sharding components.

#### Code references
* [Shutdown logic](https://github.com/mongodb/mongo/blob/2bb2f2225d18031328722f98fe05a169064a8a8a/src/mongo/db/mongod_main.cpp#L1163) for mongod.
* [Shutdown logic](https://github.com/mongodb/mongo/blob/30f5448e95114d344e6acffa92856536885e35dd/src/mongo/s/mongos_main.cpp#L336-L354) for mongos.

### Quiesce mode on shutdown
mongos enters quiesce mode prior to shutdown, to allow short-running operations to finish.
During this time, new and existing operations are allowed to run, but `isMaster`/`hello`
requests return a `ShutdownInProgress` error, to indicate that clients should start routing
operations to other nodes. Entering quiesce mode is considered a significant topology change
in the streaming `hello` protocol, so mongos tracks a `TopologyVersion`, which it increments
on entering quiesce mode, prompting it to respond to all waiting hello requests.

### helloOk Protocol Negotation

In order to preserve backwards compatibility with old drivers, mongos currently supports both
the [`isMaster`] command and the [`hello`] command. New drivers and 5.0+ versions of the server
will support `hello`. When connecting to a sharded cluster via mongos, a new driver will send
"helloOk: true" as a part of the initial handshake. If mongos supports hello, it will respond
with "helloOk: true" as well. This way, new drivers know that they're communicating with a version
of the mongos that supports `hello` and can start sending `hello` instead of `isMaster` on this
connection.

If mongos does not support `hello`, the `helloOk` flag is ignored. A new driver will subsequently
not see "helloOk: true" in the response and must continue to send `isMaster` on this connection. Old
drivers will not specify this flag at all, so the behavior remains the same.

#### Code references
* [isMaster command](https://github.com/mongodb/mongo/blob/r4.8.0-alpha/src/mongo/s/commands/cluster_is_master_cmd.cpp#L248) for mongos.
* [hello command](https://github.com/mongodb/mongo/blob/r4.8.0-alpha/src/mongo/s/commands/cluster_is_master_cmd.cpp#L64) for mongos.

# Cluster DDL operations

[Data Definition Language](https://en.wikipedia.org/wiki/Data_definition_language) (DDL) operations are operations that change the metadata;
some examples of DDLs are create/drop database or create/rename/drop collection.

Metadata are tracked in the two main MongoDB catalogs:
- *[Local catalog](https://github.com/mongodb/mongo/blob/master/src/mongo/db/catalog/README.md#the-catalog)*: present on each shard, keeping
track of databases/collections/indexes the shard owns or has knowledge of.
- *Sharded Catalog*: residing on the config server, keeping track of the metadata of databases and sharded collections for which it serves
as the authoritative source of information.

## Sharding DDL Coordinator
The [ShardingDDLCoordinator](https://github.com/mongodb/mongo/blob/106b96548c5214a8e246a1cf6ac005a3985c16d4/src/mongo/db/s/sharding_ddl_coordinator.h#L47-L191)
is the main component of the DDL infrastructure for sharded clusters: it is an abstract class whose concrete implementations have the
responsibility of coordinating the different DDL operations between shards and the config server in order to keep the two catalogs
consistent. When a DDL request is received by a router, it gets forwarded to the [primary shard](https://docs.mongodb.com/manual/core/sharded-cluster-shards/#primary-shard)
of the targeted database. For the sake of clarity, createDatabase is the only DDL operation that cannot possibly get forwarded to the
database primary but is instead routed to the config server, as the database may not exist yet.

##### Serialization and joinability of DDL operations
When a primary shard receives a DDL request, it tries to construct a DDL coordinator performing the following steps:
- Acquire the [distributed lock for the database](https://github.com/mongodb/mongo/blob/908e394d39b223ce498fde0d40e18c9200c188e2/src/mongo/db/s/sharding_ddl_coordinator.cpp#L155). This ensures that at most one DDL operation at a time will run for namespaces belonging to the same database on that particular primary node.
- Acquire the distributed lock for the [collection](https://github.com/mongodb/mongo/blob/908e394d39b223ce498fde0d40e18c9200c188e2/src/mongo/db/s/sharding_ddl_coordinator.cpp#L171) (or [collections](https://github.com/mongodb/mongo/blob/908e394d39b223ce498fde0d40e18c9200c188e2/src/mongo/db/s/sharding_ddl_coordinator.cpp#L181)) involved in the operation.

In case a new DDL petition on the same namespace gets forwarded by a router while a DDL coordinator is instantiated, a [check is performed](https://github.com/mongodb/mongo/blob/b7a055f55a202ba870730fb865579acf5d9fb90f/src/mongo/db/s/sharding_ddl_coordinator.h#L54-L61)
on the shard in order to join the ongoing operation if the options match (same operation with same parameters) or fail if they don't
(different operation or same operation with different parameters).

##### Execution of DDL coordinators
Once the distributed locks have been acquired, it is guaranteed that no other concurrent DDLs are happening for the same database,
hence a DDL coordinator can safely start [executing the operation](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/sharding_ddl_coordinator.cpp#L207).

As first step, each coordinator is required to [majority commit a document](https://github.com/mongodb/mongo/blob/2ae2bcedfb7d48e64843dd56b9e4f107c56944b6/src/mongo/db/s/sharding_ddl_coordinator.h#L105-L116) -
that we will refer to as state document - containing all information regarding the running operation such as name of the DDL, namespaces
involved and other metadata identifying the original request. At this point, the coordinator is entitled to start making both local and
remote catalog modifications, eventually after blocking CRUD operations on the changing namespaces; when the execution reaches relevant
points, the state can be checkpointed by [updating the state document](https://github.com/mongodb/mongo/blob/b7a055f55a202ba870730fb865579acf5d9fb90f/src/mongo/db/s/sharding_ddl_coordinator.h#L118-L127).

The completion of a DDL operation is marked by the [state document removal](https://github.com/mongodb/mongo/blob/b7a055f55a202ba870730fb865579acf5d9fb90f/src/mongo/db/s/sharding_ddl_coordinator.cpp#L258)
followed by the [release of the distributed locks](https://github.com/mongodb/mongo/blob/b7a055f55a202ba870730fb865579acf5d9fb90f/src/mongo/db/s/sharding_ddl_coordinator.cpp#L291-L298)
in inverse order of acquisition.

Some DDL operations are required to block migrations before actually executing so that the coordinator has a consistent view of which
shards contain data for the collection. The [setAllowMigration command](https://github.com/mongodb/mongo/blob/c5fd926e176fcaf613d9fb785f5bdc70e1aa14be/src/mongo/db/s/config/configsvr_set_allow_migrations_command.cpp#L42)
serves the purpose of blocking ongoing migrations and avoiding new ones to start.

##### Resiliency to elections, crashes and errors

DDL coordinators are resilient to elections and sudden crashes because they're instances of a [primary only service](https://github.com/mongodb/mongo/blob/master/docs/primary_only_service.md)
that - by definition - gets automatically resumed when the node of a shard steps up.

The coordinator state document has a double aim:
- It serves the purpose of primary only service state document.
- It tracks the progress of a DDL operation.

Steps executed by coordinators are implemented in idempotent phases. When entering a phase, the state is checkpointed as majority committed
on the state document before actually executing the phase. If a node fails or steps down, it is then safe to resume the DDL operation as
follows: skip previous phases and re-execute starting from the checkpointed phase.

When a new primary node is elected, the DDL primary only service is [rebuilt](https://github.com/mongodb/mongo/blob/20549d58943b586749d1570eee834c71bdef1b37/src/mongo/db/s/sharding_ddl_coordinator_service.cpp#L158-L185)
resuming outstanding coordinators, if present; during this recovery phase, incoming DDL operations are [temporarily put on hold](https://github.com/mongodb/mongo/blob/20549d58943b586749d1570eee834c71bdef1b37/src/mongo/db/s/sharding_ddl_coordinator_service.cpp#L152-L156)
waiting for pre-existing DDL coordinators to be re-instantiated in order to avoid conflicts in the acquisition of the distributed locks.

If a [recoverable error](https://github.com/mongodb/mongo/blob/a1752a0f5300b3a4df10c0a704c07e597c3cd291/src/mongo/db/s/sharding_ddl_coordinator.cpp#L216-L226)
is caught at execution-time, it will be retried indefinitely; all other errors errors have the effect of stopping and destructing the DDL coordinator and -
because of that - are never expected to happen after a coordinator performs destructive operations.

# User Write Blocking

User write blocking prevents user initiated writes from being performed on C2C source and destination
clusters during certain phases of C2C replication, allowing durable state to be propagated from the
source without experiencing conflicts. Because the source and destination clusters are different
administrative domains and thus can have separate configurations and metadata, operations which
affect metadata, such as replset reconfig, are permitted. Also, internal operations which affect user
collections but leave user data logically unaffected, such as chunk migration, are still permitted.
Finally, users with certain privileges can bypass user write blocking; this is necessary so that the
C2C sync daemon itself can write to user data.

User write blocking is enabled and disabled by the command `{setUserWriteBlockMode: 1, global:
<true/false>}`. On replica sets, this command is invoked on the primary, and enables/disables user
write blocking replica-set-wide. On sharded clusters, this command is invoked on `mongos`, and
enables/disables user write blocking cluster-wide. We define a write as a "user write" if the target
database is not internal (the `admin`, `local`, and `config` databases being defined as internal),
and if the user that initiated the write cannot perform the `bypassWriteBlockingMode` action on the
`cluster` resource. By default, only the `restore`, `root`, and `__system` built-in roles have this
privilege.

The `UserWriteBlockModeOpObserver` is responsible for blocking disallowed writes. Upon any operation
which writes, this `OpObserver` checks whether the `GlobalUserWriteBlockState` [allows writes to the
target
namespace](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/op_observer/user_write_block_mode_op_observer.cpp#L276-L283).
The `GlobalUserWriteBlockState` stores whether user write blocking is enabled in a given
`ServiceContext`. As part of its write access check, it [checks whether the `WriteBlockBypass`
associated with the given `OperationContext` is
enabled](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/s/global_user_write_block_state.cpp#L59-L67).
The `WriteBlockBypass` stores whether the user that initiated the write is able to perform writes
when user write blocking is enabled.  On internal requests (i.e. from other `mongod` or `mongos`
instances in the sharded cluster/replica set), the request originator propagates `WriteBlockBypass`
[through the request
metadata](https://github.com/10gen/mongo/blob/182616b7b45a1e360839c612c9ee8acaa130fe17/src/mongo/rpc/metadata.cpp#L115).
On external requests, `WriteBlockBypass` is enabled [if the authenticated user is privileged to
bypass user
writes](https://github.com/10gen/mongo/blob/07c3d2ebcd3ca8127ed5a5aaabf439b57697b530/src/mongo/db/write_block_bypass.cpp#L60-L63).
The `AuthorizationSession`, which is responsible for maintaining the authorization state, keeps track
of whether the user has the privilege to bypass user write blocking by [updating a cached variable
upon any changes to the authorization
state](https://github.com/10gen/mongo/blob/e4032fe5c39f1974c76de4cefdc07d98ab25aeef/src/mongo/db/auth/authorization_session_impl.cpp#L1119-L1121).
This structure enables, for example, sharded writes to work correctly with user write blocking,
because the `WriteBlockBypass` state is initially set on the `mongos` based on the
`AuthorizationSession`, which knows the privileges of the user making the write request, and then
propagates from the `mongos` to the shards involved in the write. Note that this means on requests
from `mongos`, shard servers don't check their own `AuthorizationSession`s when setting
`WriteBlockBypass`. This would be incorrect behavior since internal requests have internal
authorization, which confers all privileges, including the privilege to bypass user write blocking.

The `setUserWriteBlockMode` command, before enabling user write blocking, blocks creation of new
index builds and aborts all currently running index builds on non-internal databases, and drains the
index builds it cannot abort. This upholds the invariant that while user write blocking is enabled,
all running index builds are allowed to bypass write blocking and therefore can commit without
additional checks.

In sharded clusters, enabling user write blocking is a two-phase operation, coordinated by the config
server. The first phase disallows creation of new `ShardingDDLCoordinator`s and drains all currently
running `DDLCoordinator`s. The config server waits for all shards to complete this phase before
moving onto the second phase, which aborts index builds and enables write blocking. This structure is
used because enabling write blocking while there are ongoing `ShardingDDLCoordinator`s would prevent
those operations from completing.

#### Code references
* The [`UserWriteBlockModeOpObserver`
  class](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/op_observer/user_write_block_mode_op_observer.h#L40)
* The [`GlobalUserWriteBlockState`
  class](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/s/global_user_write_block_state.h#L37)
* The [`WriteBlockBypass`
  class](https://github.com/10gen/mongo/blob/07c3d2ebcd3ca8127ed5a5aaabf439b57697b530/src/mongo/db/write_block_bypass.h#L38)
* The [`abortUserIndexBuildsForUserWriteBlocking`
  function](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/index_builds_coordinator.cpp#L850),
  used to abort and drain all current user index builds
* The [`SetUserWriteBlockModeCoordinator`
  class](https://github.com/10gen/mongo/blob/ce908a66890bcdd87e709b584682c6b3a3a851be/src/mongo/db/s/config/set_user_write_block_mode_coordinator.h#L38),
  used to coordinate the `setUserWriteBlockMode` command for sharded clusters
* The [`UserWritesRecoverableCriticalSectionService`
  class](https://github.com/10gen/mongo/blob/1c4e5ba241829145026f8aa0db70707f15fbe7b3/src/mongo/db/s/user_writes_recoverable_critical_section_service.h#L88),
  used to manage and persist the user write blocking state
* The `setUserWriteBlockMode` command invocation:
    - [On a non-sharded
      `mongod`](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/commands/set_user_write_block_mode_command.cpp#L68)
    - [On a shard
      server](https://github.com/10gen/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/s/shardsvr_set_user_write_block_mode_command.cpp#L61)
    - [On a config
      server](https://github.com/10gen/mongo/blob/c96f8dacc4c71b4774c932a07be4fac71b6db628/src/mongo/db/s/config/configsvr_set_user_write_block_mode_command.cpp#L56)
    - [On a
      `mongos`](https://github.com/10gen/mongo/blob/4ba31bc8627426538307848866d3165a17aa29fb/src/mongo/s/commands/cluster_set_user_write_block_mode_command.cpp#L61)
