# Sharded cluster topology

A sharded cluster consists of:

- A **config server replica set** (CSRS): stores the cluster's metadata.
- A set of **shards**: bearing the data.
- A set of **routers** (mongos): route queries and operations to the appropriate shards, as
  instructed by the metadata in the CSRS.

## Deployment requirements by role

| Role         | Allowed topology                    | Required setup                                                                                                                           |
| ------------ | ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| CSRS         | Replica set **only**                | Start with `--configsvr --replSet`; a single-node CSRS is allowed                                                                        |
| Shard        | Replica set or standalone           | Start with `--shardsvr`                                                                                                                  |
| Router       | `mongos`                            | Stateless; provide a valid config-server connection string at startup                                                                    |
| Config shard | CSRS acting as a data bearing shard | Use `transitionFromDedicatedConfigServer` and `transitionToDedicatedConfigServer` to transition between dedicated and data-bearing roles |

## Sources of topology information

### Routers

The set of router nodes in the cluster cannot be authoritatively queried. Routers do periodically
write to `config.mongos`, and that collection can be used to get an approximate idea of how many
routers there are in the cluster.

### Config server nodes

Shards are initialized with a connection string to the CSRS, which is persisted to the shard
identity document (in the shard's `admin.system.version`). Each shard spawns a
[`ReplicaSetMonitor`](/src/mongo/client/README.md#replica-set-monitoring-and-host-targeting) for the
CSRS, and will persist newer versions of the CSRS connection string to the shard identity document.

Routers are also started up with a connection string to the CSRS, but are stateless and it is up to
the deployment manager to ensure the router is restarted with a valid connection string. Replica set
topology changes that happen on the config server during a router's runtime are also tracked via the
`ReplicaSetMonitor`, but those are not persisted.

### Shards

The set of shards (and their properties) in the cluster is authoritatively persisted in
`config.shards`. `TypeShard` encodes the format of each entry in the collection. Entries contain
these properties:

- Name
- UUID
- Host (connection string)
- Draining
- Tags
- TopologyTime
- ReplSetConfigVersion

Each shard is uniquely identified by a name and a UUID. The name can be user supplied or derived
from the replica set name. The UUID is generated when the shard is added to the cluster. The
difference between the name and the UUID is that the name can be reused if a shard is removed and
then added back, while the UUID is unique to each incarnation of a shard.

A config-shard (data-bearing) will be listed as a shard in `config.shards`, while a dedicated config
server will not be listed. When listed, the CSRS is identified by the reserved `config` name (as
defined in `ShardId::kConfigServerId`) and a reserved UUID (as defined in
`ShardType::kConfigServerUuid`).

## Topology changes and the TopologyTime

A versioning scheme is encoded in the `topologyTime` in `config.shards`, and is used by our caching
mechanism (the `ShardRegistry`). The highest `topologyTime` value among all the entries in the
collection is considered the cluster's `topologyTime`.

We can think of topology changes mainly as changes in `config.shards` which advance the
`topologyTime`. Specifically:

- Adding a shard
- Removing a shard

Any changes to properties that are cached in the `ShardRegistry` must be accompanied by a
corresponding `topologyTime` bump (see TODO SERVER-110328 for a scenario where this is currently not
respected).

Other changes to `config.shards` that do **not** advance the `topologyTime` are:

- Setting the “draining” state
- addShardToZone / removeShardFromZone

These don't need to advance the `topologyTime` because the `ShardRegistry` does not cache those
properties.

Knowledge about topology changes is propagated through the cluster via gossiping of the
`topologyTime`.

### TopologyTimeTicker

Topology changes are susceptible to replication rollback. As such, it would be incorrect to gossip
out a `topologyTime` that is yet to be majority committed. The `TopologyTimeTicker` tracks new
topology times and waits for them to be majority committed before allowing them to be gossiped out
from the config server.

### Updating a shard's connection string

Upon `replSetReconfig` (and step-up) the shard's primary schedules an asynchronous connection string
update to `config.shards`. Updates use the config's `setVersion` to prevent overwriting newer
connection strings.

## The ShardRegistry

The `ShardRegistry` is present in all nodes in the cluster (mongos, mongod), and is an eventually
consistent cache of `config.shards`.

The registry instantiates a `Shard` instance for each of the shards in the cluster, and an instance
to the CSRS node. Importantly, it also keeps a `Shard` lookup map keyed by:

- uuid
- shard name
- replica set name
- connection string
- host and port

Spawning a new `Shard` comes associated with creating the associated `ReplicaSetMonitor` (assuming
the standard replica set shard deployment).

The registry is implemented on top of the `ReadThroughCache`. Cache refreshes are mainly driven by
the discovery of new `topologyTime`s, but also by notifications of new connection strings provided
by the `ReplicaSetMonitor`s. In consequence, the `ShardRegistry::Time` used to drive the
`ReadThroughCache` consists of:

- ForceReloadIncrement
- TopologyTime

`ReplicaSetMonitor` updates are incorporated by storing the latest reported connection string and
forcing a refresh, during the refresh the registry gives priority to RSM reported connection
strings.

Given that connection string updates are also written to `config.shards` by the primary of each
shard, it might seem redundant to receive updates from the `ReplicaSetMonitor`. However, in a
scenario where the CSRS has no primary, or it is not reachable, it is still valuable to have an
alternative means by which other nodes' `ShardRegistry` can incorporate replica set
reconfigurations.

### ReplicaSetMonitor

Each `Shard` instance is associated with a `ReplicaSetMonitor` (unless the shard is a standalone
node) that tracks the state of the shard's replica set. The `ReplicaSetMonitor` is responsible for
discovering new members, tracking their health, and notifying the `ShardRegistry` of any changes in
the connection string. `ShardRegistry` refreshes create new `Shard` instances, but the underlying
`ReplicaSetMonitor` is reused (unless explicitly destroyed and recreated in certain edge cases).

### Periodic ping

To prevent the ShardRegistry from remaining stale for too long, a periodic background job pings the
config server every 30 seconds. The ping's response contains the latest `topologyTime` as part of
the gossip metadata. Only when a newer `topologyTime` is discovered, a refresh is scheduled.

### Known issues

- All `ShardRegistry` refreshes involve going to the CSRS. Even if new information is available
  through the `ReplicaSetMonitor` if the `config.shards` query cannot be completed, those won't be
  incorporated. This was a trade-off taken to simplify the refresh code.
- It is currently not possible to disambiguate re-incarnations of a shard, i.e. remove and add shard
  with the same name. However, it is unlikely that a `ShardRegistry` does not perform a refresh that
  would first see the shard removal, thanks to the periodic ping. See TODO SERVER-115711.
- Shard primary `config.shards` writes to update the connection string do not bump the
  `topologyTime`. This can cause a scenario where the `ShardRegistry` is not aware that it needs to
  refresh. See TODO SERVER-110328.
- In the event that a node is unable to reach any of the current members of a shard replica, even if
  new (reachable) members are discovered via `config.shards` query, the RSM would not be aware of
  them and thus can't be targeted. The current `ReplicaSetMonitor` API does not allow notifying new
  nodes discovered by other means, and destroying and recreating the RSM on each ShardRegistry
  refresh would somewhat defeat the purpose of having the `ReplicaSetMonitor` tracking
  reconfigurations in the first place. In the case that this situation lasts an extended period of
  time and all nodes end up being replaced, the `ReplicaSetMonitor` will not be able to recover. See
  TODO SERVER-115725.

### Recovery

The `_flushShardRegistry` command can be used to reset and refresh the `ShardRegistry` of a node.
This is useful in scenarios where the `ShardRegistry` has become non-functional due to a bug. The
command will clear the current cache and force a refresh from the config server, ensuring that the
node has the most up-to-date information about the cluster's topology.
