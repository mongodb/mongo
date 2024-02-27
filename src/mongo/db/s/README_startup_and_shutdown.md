# Node startup and shutdown

## Startup and sharding component initialization

The mongod intialization process is split into three phases. The first phase runs on startup and initializes the set of stateless components based on the cluster role. The second phase then initializes additional components that must be initialized with state read from the config server. The third phase is run on the [transition to primary](https://github.com/mongodb/mongo/blob/879d50a73179d0dd94fead476468af3ee4511b8f/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L822-L901) and starts services that only run on primaries.

### Shard Server initialization

#### Phase 1:

1. On a shard server, the `CollectionShardingState` factory is set to an instance of the `CollectionShardingStateFactoryShard` implementation. The component lives on the service context.
1. The sharding [OpObservers are created](https://github.com/mongodb/mongo/blob/0e08b33037f30094e9e213eacfe16fe88b52ff84/src/mongo/db/mongod_main.cpp#L1000-L1001) and registered with the service context. The `MigrationChunkClonerSourceOpObserver` class forwards operations during migration to the chunk cloner. The `ShardServerOpObserver` class is used to handle the majority of sharding related events. These include loading the shard identity document when it is inserted and performing range deletions when they are marked as ready.

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

-   Function to [initialize global sharding state](https://github.com/mongodb/mongo/blob/eeca550092d9601d433e04c3aa71b8e1ff9795f7/src/mongo/s/sharding_initialization.cpp#L188-L237).
-   Function to [initialize sharding environment](https://github.com/mongodb/mongo/blob/37ff80f6234137fd314d00e2cd1ff77cde90ce11/src/mongo/db/s/sharding_initialization_mongod.cpp#L255-L286) on shard server.
-   Hook for sharding [transition to primary](https://github.com/mongodb/mongo/blob/879d50a73179d0dd94fead476468af3ee4511b8f/src/mongo/db/repl/replication_coordinator_external_state_impl.cpp#L822-L901).

## Shutdown

If the mongod server is primary, it will [try to step down](https://github.com/mongodb/mongo/blob/0987c120f552ab6d347f6b1b6574345e8c938c32/src/mongo/db/mongod_main.cpp#L1046-L1072). Mongod and mongos then run their respective shutdown tasks which cleanup the remaining sharding components.

#### Code references

-   [Shutdown logic](https://github.com/mongodb/mongo/blob/2bb2f2225d18031328722f98fe05a169064a8a8a/src/mongo/db/mongod_main.cpp#L1163) for mongod.
-   [Shutdown logic](https://github.com/mongodb/mongo/blob/30f5448e95114d344e6acffa92856536885e35dd/src/mongo/s/mongos_main.cpp#L336-L354) for mongos.

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

When mongos establishes outgoing connections to mongod nodes in the cluster, it always uses `hello`
rather than `isMaster`.

#### Code references

-   [isMaster command](https://github.com/mongodb/mongo/blob/r4.8.0-alpha/src/mongo/s/commands/cluster_is_master_cmd.cpp#L248) for mongos.
-   [hello command](https://github.com/mongodb/mongo/blob/r4.8.0-alpha/src/mongo/s/commands/cluster_is_master_cmd.cpp#L64) for mongos.
