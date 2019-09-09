// Tests that the $changeStream stage returns an error when run against a standalone mongod.
// @tags: [requires_sharding, uses_change_streams]

(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.
// For supportsMajorityReadConcern().
load("jstests/multiVersion/libs/causal_consistency_helpers.js");

// Skip this test if running with --nojournal and WiredTiger.
if (jsTest.options().noJournal &&
    (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
    print("Skipping test because running WiredTiger without journaling isn't a valid" +
          " replica set configuration");
    return;
}

if (!supportsMajorityReadConcern()) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    return;
}

function assertChangeStreamNotSupportedOnConnection(conn) {
    const notReplicaSetErrorCode = 40573;
    assertErrorCode(conn.getDB("test").non_existent, [{$changeStream: {}}], notReplicaSetErrorCode);
    assertErrorCode(conn.getDB("test").non_existent,
                    [{$changeStream: {fullDocument: "updateLookup"}}],
                    notReplicaSetErrorCode);
}

const conn = MongoRunner.runMongod({enableMajorityReadConcern: ""});
assert.neq(null, conn, "mongod was unable to start up");
// $changeStream cannot run on a non-existent database.
assert.commandWorked(conn.getDB("test").ensure_db_exists.insert({}));
assertChangeStreamNotSupportedOnConnection(conn);
assert.eq(0, MongoRunner.stopMongod(conn));

// Test a sharded cluster with standalone shards.
// TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
const clusterWithStandalones = new ShardingTest({
    shards: 2,
    other: {shardOptions: {enableMajorityReadConcern: ""}},
    config: 1,
    shardAsReplicaSet: false
});
// Make sure the database exists before running any commands.
const mongosDB = clusterWithStandalones.getDB("test");
// enableSharding will create the db at the cluster level but not on the shards. $changeStream
// through mongoS will be allowed to run on the shards despite the lack of a database.
assert.commandWorked(mongosDB.adminCommand({enableSharding: "test"}));
assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.s);
// Shard the 'ensure_db_exists' collection on a hashed key before running $changeStream on the
// shards directly. This will ensure that the database is created on both shards.
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: "test.ensure_db_exists", key: {_id: "hashed"}}));
assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.shard0);
assertChangeStreamNotSupportedOnConnection(clusterWithStandalones.shard1);
clusterWithStandalones.stop();
}());
