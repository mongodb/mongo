/**
 * Tests that mongos can start in config-only mode against a standby cluster that was originally
 * configured with an embedded config server (configShard mode).
 *
 * @tags: [
 *   requires_sharding,
 *   requires_persistence,
 * ]
 */

import {StandbyClusterTestFixture} from "jstests/noPassthrough/libs/sharded_cluster_topology/standby_cluster_test_fixture.js";

const fixture = new StandbyClusterTestFixture({
    name: jsTestName(),
    shards: 2,
    rs: {nodes: 3},
    configShard: true,
});

assert.commandWorked(fixture.st.s.getDB("unshardedDB").getCollection("test").insertOne({x: 1}));
assert.commandWorked(fixture.st.s.getDB("shardedDB").getCollection("test").insertOne({x: 1}));
assert.commandWorked(fixture.st.s.adminCommand({shardCollection: "shardedDB.test", key: {_id: 1}}));

fixture.transitionToStandby();

const mongos = MongoRunner.runMongos({
    configdb: fixture.standbyRS.getURL(),
    configOnly: "",
});
assert.neq(null, mongos, "mongoS failed to start up against standby config server");

assert.commandWorked(mongos.adminCommand({hello: 1}));

// Verify that catalog metadata from the original embedded config server survived the transition
// by reading directly from the standby replica set (mongos in config-only mode blocks routing
// to the config database via the shard list refresh path).
{
    const standbyPrimary = fixture.standbyRS.getPrimary();
    const configDB = standbyPrimary.getDB("config");

    const dbDocs = configDB.databases
        .find({}, {_id: 1})
        .toArray()
        .map((doc) => doc._id);
    assert(dbDocs.includes("unshardedDB"), `Expected 'unshardedDB' in config.databases, got: ${tojson(dbDocs)}`);
    assert(dbDocs.includes("shardedDB"), `Expected 'shardedDB' in config.databases, got: ${tojson(dbDocs)}`);

    const shardedColls = configDB.collections.find({_id: "shardedDB.test"}).toArray();
    assert.eq(1, shardedColls.length, `Expected shardedDB.test in config.collections, got: ${tojson(shardedColls)}`);
}

// Verify that data operations are correctly rejected with config-only mode errors.
assert.commandFailedWithCode(mongos.getDB("unshardedDB").runCommand({find: "test"}), 12319007);
assert.commandFailedWithCode(mongos.getDB("shardedDB").runCommand({find: "test"}), 12319007);

MongoRunner.stopMongos(mongos);
fixture.teardown();
