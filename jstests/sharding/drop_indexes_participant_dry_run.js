/**
 * Tests the dryRun functionality of the _shardsvrDropIndexesParticipant command.
 *
 * @tags: [
 *  requires_fcv_83
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertShardHasIndexes(st, dbName, collName, expectedIndexNames) {
    let indexes = st.rs0.getPrimary().getDB(dbName)[collName].getIndexes();
    assert.sameMembers(
        indexes.map((ix) => ix.name),
        expectedIndexNames,
        tojson(indexes),
    );
}

const st = new ShardingTest({shards: 1});
const dbName = "test";
const collName = "dropIndexesDryRun";
const ns = dbName + "." + collName;

const testDB = st.s.getDB(dbName);
const coll = testDB.getCollection(collName);

assert.commandWorked(testDB.adminCommand({shardCollection: ns, key: {x: 1}}));

assert.commandWorked(
    coll.insertMany([
        {x: 1, y: 1, z: 1},
        {x: 2, y: 2, z: 2},
        {x: 3, y: 3, z: 3},
    ]),
);

assert.commandWorked(coll.createIndex({y: 1}, {name: "y_1"}));
assert.commandWorked(coll.createIndex({z: 1}, {name: "z_1"}));
assert.commandWorked(coll.createIndex({x: 1, y: 1}, {name: "x_1_y_1"})); // Shard key compatible

const shardKeyPattern = {
    x: 1,
};

// Test 1: Dry run with specific index - should succeed
jsTest.log.info("Test 1: Dry run with specific index name");
let dryRunCmd = {
    _shardsvrDropIndexesParticipant: collName,
    dbName: dbName,
    index: "y_1",
    dryRun: true,
    shardKeyPattern: shardKeyPattern,
    writeConcern: {w: "majority"},
};

let result = st.shard0.getDB(dbName).runCommand(dryRunCmd);
assert.commandWorked(result, "Dry run should succeed for valid index");
assert.eq(result.msg, "index 'y_1' would be dropped");
// Verify the index still exists after dry run
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1", "y_1", "z_1", "x_1_y_1"]);

// Test 2: Dry run with wildcard - should succeed
jsTest.log.info("Test 2: Dry run with wildcard");
dryRunCmd.index = "*";
result = st.shard0.getDB(dbName).runCommand(dryRunCmd);
assert.commandWorked(result, "Dry run should succeed for wildcard");
assert.eq(result.msg, "non-_id indexes and non-shard key indexes would be dropped for collection");
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1", "y_1", "z_1", "x_1_y_1"]);

// Test 3: Dry run trying to drop _id index - should fail
jsTest.log.info("Test 3: Dry run trying to drop _id index");
dryRunCmd.index = "_id_";
result = st.shard0.getDB(dbName).runCommand(dryRunCmd);
assert.commandFailed(result, "Command should fail when trying to drop _id index");
if (result.code === 10742100) {
    jsTest.log.info("Test 3: Collection is clustered - got expected clustered index error");
    assert(result.errmsg.includes("clusteredIndex"), "Error message should mention clusteredIndex");
} else if (result.code === ErrorCodes.InvalidOptions) {
    // Regular _id index error
    jsTest.log.info("Test 3: Collection is regular - got expected InvalidOptions error");
    assert(result.errmsg.includes("cannot drop _id index"), "Error message should mention _id index");
}

// Test 4: Dry run trying to drop non-existent index - should succeed
jsTest.log.info("Test 4: Dry run trying to drop non-existent index");
dryRunCmd.index = "nonexistent_index";
result = st.shard0.getDB(dbName).runCommand(dryRunCmd);
assert.commandWorked(result, "Dry run should succeed for non-existent index");

// Test 5: Dry run trying to drop last shard key compatible index
jsTest.log.info("Test 5: Dry run trying to drop shard key compatible index");
// First drop other shard key compatible indexes to make x_1 the last compatible one
assert.commandWorked(st.shard0.getDB(dbName).runCommand({dropIndexes: collName, index: "x_1_y_1"}));

dryRunCmd.index = "x_1";
result = st.shard0.getDB(dbName).runCommand(dryRunCmd);
assert.commandFailedWithCode(
    result,
    ErrorCodes.CannotDropShardKeyIndex,
    "Should fail if trying to drop the last shard key compatible index",
);
// Verify index still exists after failed dry run
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1", "y_1", "z_1"]);

// Test 6: Dry run with key pattern instead of name
jsTest.log.info("Test 6: Dry run with key pattern");
dryRunCmd.index = {
    y: 1,
};
result = st.shard0.getDB(dbName).runCommand(dryRunCmd);
assert.commandWorked(result, "Dry run should succeed for valid key pattern");
assert.eq(result.msg, "index 'y_1' would be dropped");

// Test 7: Dry run without dryRun parameter (should perform actual drop)
jsTest.log.info("Test 7: Command without dryRun parameter");
let normalCmd = {
    _shardsvrDropIndexesParticipant: collName,
    dbName: dbName,
    index: "y_1",
    // dryRun: false,
    txnNumber: NumberLong(1),
    shardKeyPattern: shardKeyPattern,
    writeConcern: {w: "majority"},
};

result = st.shard0.getDB(dbName).runCommand(normalCmd);
assert.commandWorked(result, "Normal command should succeed");
// Verify the index was actually dropped
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1", "z_1"]);

// Test 8: Dry run without shardKeyPattern parameter
// Should fail since x_1 is last compatible shard key index, but succeeds
// because no shardKeyPattern is passed so validation is skipped
jsTest.log.info("Test 8: Dry run without shardKeyPattern parameter");
let dryRunNoShardKey = {
    _shardsvrDropIndexesParticipant: collName,
    dbName: dbName,
    index: "x_1", // This is the last compatible index
    dryRun: true,
    // shardKeyPattern: shardKeyPattern,  // Not specified
    writeConcern: {w: "majority"},
};

result = st.shard0.getDB(dbName).runCommand(dryRunNoShardKey);
assert.commandWorked(result, "Dry run should succeed without shardKeyPattern");
assert.eq(result.msg, "index 'x_1' would be dropped");

// Test 9: Dry run on non-existent collection - should fail
jsTest.log.info("Test 9: Dry run on non-existent collection");
let nonExistentCmd = {
    _shardsvrDropIndexesParticipant: "nonexistent",
    dbName: dbName,
    index: "some_index",
    dryRun: true,
    writeConcern: {w: "majority"},
};

result = st.shard0.getDB(dbName).runCommand(nonExistentCmd);
assert.commandFailedWithCode(result, ErrorCodes.NamespaceNotFound, "Should fail for non-existent collection");

st.stop();
