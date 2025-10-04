# Fixtures

Fixtures define a specific topology that tests run against.

## Supported Fixtures

Specify any of the following as the `fixture` in your [Suite](../../../../buildscripts/resmokeconfig/suites/README.md) config:

- [`BulkWriteFixture`](./bulk_write.py) - Fixture which provides JSTests with a set of clusters to run tests against.
- [`ExternalFixture`](./external.py) - Fixture which provides JSTests capability to connect to external (non-resmoke) cluster.
- [`ExternalShardedClusterFixture`](./shardedcluster.py) - Fixture to interact with external sharded cluster fixture.
- [`MongoDFixture`](./standalone.py) - Fixture which provides JSTests with a standalone mongod to run against.
- [`MongoTFixture`](./mongot.py) - Fixture which provides JSTests with a mongot to run alongside a mongod.
- [`MultiReplicaSetFixture`](./multi_replica_set.py) - Fixture which provides JSTests with a set of replica sets to run against.
- [`MultiShardedClusterFixture`](./multi_sharded_cluster.py) - Fixture which provides JSTests with a set of sharded clusters to run against.
- [`ReplicaSetFixture`](./replicaset.py) - Fixture which provides JSTests with a replica set to run against.
- [`ShardedClusterFixture`](./shardedcluster.py) - Fixture which provides JSTests with a sharded cluster to run against.
  - Used when the MongoDB deployment is started by the JavaScript test itself with `MongoRunner`, `ReplSetTest`, or `ShardingTest`.
- [`YesFixture`](./yesfixture.py) - Fixture which spawns several `yes` executables to generate lots of log messages.

## Interfaces

- [`Fixture`](./interface.py) - Base class for all fixtures.
- [`MultiClusterFixture`](./interface.py) - Base class for fixtures that may consist of multiple independent participant clusters.
  - The participant clusters can function independently without coordination, but are bound together only for some duration as they participate in some process such as a migration. The participant clusters are fixtures themselves.
- [`NoOpFixture`](./interface.py) - A Fixture implementation that does not start any servers.
- [`ReplFixture`](./interface.py) - Base class for all fixtures that support replication.
