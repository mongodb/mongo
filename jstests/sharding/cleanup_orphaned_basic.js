// Basic tests of cleanupOrphaned. Validates that non allowed uses of the cleanupOrphaned
// command fail.
//
// requires_persistence because it restarts a shard.
// @tags: [requires_persistence]

import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test restarts a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

if (!jsTestOptions().useAutoBootstrapProcedure) {
    // TODO: SERVER-80318 Remove block
    /*****************************************************************************
     * Unsharded mongod.
     ****************************************************************************/

    // cleanupOrphaned fails against unsharded mongod.
    let mongod = MongoRunner.runMongod();
    assert.commandFailed(mongod.getDB("admin").runCommand({cleanupOrphaned: "foo.bar"}));
    MongoRunner.stopMongod(mongod);
}

/*****************************************************************************
 * Bad invocations of cleanupOrphaned command.
 ****************************************************************************/

let st = new ShardingTest({other: {rs: true, rsOptions: {nodes: 2}}});

let mongos = st.s0;
let mongosAdmin = mongos.getDB("admin");
let dbName = "foo";
let collectionName = "bar";
let ns = dbName + "." + collectionName;

assert.commandWorked(mongosAdmin.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
let coll = mongos.getCollection(ns);

// cleanupOrphaned fails against mongos ('no such command'): it must be run
// on mongod.
assert.commandFailed(mongosAdmin.runCommand({cleanupOrphaned: ns}));

// cleanupOrphaned must be run on admin DB.
let shardFooDB = st.shard0.getDB(dbName);
assert.commandFailed(shardFooDB.runCommand({cleanupOrphaned: ns}));

// Must be run on primary.
let secondaryAdmin = st.rs0.getSecondary().getDB("admin");
let response = secondaryAdmin.runCommand({cleanupOrphaned: ns});
print("cleanupOrphaned on secondary:");
printjson(response);
assert.commandFailed(response);

let shardAdmin = st.shard0.getDB("admin");
let badNS = ' \\/."*<>:|?';
assert.commandFailed(shardAdmin.runCommand({cleanupOrphaned: badNS}));

// cleanupOrphaned works on sharded collection.
assert.commandWorked(mongosAdmin.runCommand({shardCollection: ns, key: {_id: 1}}));

assert.commandWorked(shardAdmin.runCommand({cleanupOrphaned: ns}));

/*****************************************************************************
 * Bad startingFromKeys.
 ****************************************************************************/

function testBadStartingFromKeys(shardAdmin) {
    // startingFromKey of MaxKey.
    response = shardAdmin.runCommand({cleanupOrphaned: ns, startingFromKey: {_id: MaxKey}});
    assert.commandWorked(response);
    assert.eq(null, response.stoppedAtKey);

    // startingFromKey doesn't match number of fields in shard key.
    assert.commandFailed(
        shardAdmin.runCommand({cleanupOrphaned: ns, startingFromKey: {someKey: "someValue", someOtherKey: 1}}),
    );

    // startingFromKey matches number of fields in shard key but not field names.
    assert.commandFailed(shardAdmin.runCommand({cleanupOrphaned: ns, startingFromKey: {someKey: "someValue"}}));

    let coll2 = mongos.getCollection("foo.baz");

    assert.commandWorked(mongosAdmin.runCommand({shardCollection: coll2.getFullName(), key: {a: 1, b: 1}}));

    // startingFromKey doesn't match number of fields in shard key.
    assert.commandFailed(
        shardAdmin.runCommand({cleanupOrphaned: coll2.getFullName(), startingFromKey: {someKey: "someValue"}}),
    );

    // startingFromKey matches number of fields in shard key but not field names.
    assert.commandFailed(
        shardAdmin.runCommand({cleanupOrphaned: coll2.getFullName(), startingFromKey: {a: "someValue", c: 1}}),
    );
}

// Note the 'startingFromKey' parameter is validated FCV is 4.4+, but is not otherwise used (in
// FCV 4.4+, cleanupOrphaned waits for there to be no orphans in the entire key space).
testBadStartingFromKeys(shardAdmin);

st.stop();
