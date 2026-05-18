/**
 * Tests dryRun behavior for aborting in-progress index builds via _shardsvrDropIndexesParticipant
 * issued against a real shard server.
 *
 * @tags: [
 * requires_sharding,
 * requires_fcv_83,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
const dbName = jsTestName();
const collName = "test";
const shardPrimary = st.shard0.rs.getPrimary();
const shardDB = shardPrimary.getDB(dbName);

const db = st.s.getDB(dbName);
const coll = db.getCollection(collName);

coll.drop();
assert.commandWorked(db.createCollection(collName));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({b: 1}));
assert.commandWorked(coll.insert({c: 1}));
assert.commandWorked(coll.createIndex({a: 1}));

IndexBuildTest.pauseIndexBuilds(shardPrimary);

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(shardPrimary, coll.getFullName(), {b: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(shardDB, collName, "b_1");

const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(shardPrimary, coll.getFullName(), {c: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
]);
IndexBuildTest.waitForIndexBuildToScanCollection(shardDB, collName, "c_1");

IndexBuildTest.assertIndexes(
    coll,
    /*numIndexes=*/ 4,
    /*readyIndexes=*/ ["_id_", "a_1"],
    /*notReadyIndexes=*/ ["b_1", "c_1"],
);

// All commands below run directly on the primary node of the (only) shard.
function runDryRunCmd(indexArg) {
    return shardDB.runCommand({
        _shardsvrDropIndexesParticipant: collName,
        dbName: dbName,
        index: indexArg,
        dryRun: true,
        writeConcern: {w: "majority"},
    });
}

// Test 1: Dry run with wildcard should work (builds in progress)
jsTest.log("Test 1: Dry run with wildcard ('*'), index builds are in progress");
let result = runDryRunCmd("*");
assert.commandWorked(result, "Dry run wildcard should work if any index build is in progress");

// Test 2: Dry run by explicit in-progress name should work
jsTest.log("Test 2: Dry run for single in-progress index name ('b_1')");
result = runDryRunCmd("b_1");
assert.commandWorked(result, "Dry run for index name with in-progress build should work");

// Test 3: Dry run for ready index (should succeed)
jsTest.log("Test 3: Dry run for ready index ('a_1')");
result = runDryRunCmd("a_1");
assert.commandWorked(result, "Dry run for ready index should work");
assert.eq(result.msg, "index 'a_1' would be dropped");

// Test 4: Dry run for a mix of ready and not-ready indexes (should fail with IndexNotFound)
jsTest.log("Test 4: Dry run for multiple indexes where one is ready ('a_1') and one not('b_1')");
result = runDryRunCmd(["a_1", "b_1"]);
assert.commandFailedWithCode(
    result,
    ErrorCodes.IndexNotFound,
    "Dry run for multiple indexes (one ready, one not) should fail",
);

// Test 5: Dry run for non-existent index (should succeed and do
// nothing)
jsTest.log("Test 5: Dry run for non-existent index");
result = runDryRunCmd("nonexistent_index");
assert.commandWorked(result, "Dry run for non-existent index should succeed");

// Test 6: Dry run for multiple in-progress indexes (should fail)
jsTest.log("Test 6: Dry run for multiple in-progress index names (['b_1', 'c_1'])");
result = runDryRunCmd(["b_1", "c_1"]);
assert.commandFailedWithCode(result, ErrorCodes.IndexNotFound, "Dry run for multiple in-progress indexes should fail");

IndexBuildTest.resumeIndexBuilds(shardPrimary);
awaitFirstIndexBuild();
awaitSecondIndexBuild();

IndexBuildTest.assertIndexes(
    coll,
    /*numIndexes=*/ 4,
    /*readyIndexes=*/ ["_id_", "a_1", "b_1", "c_1"],
    /*notReadyIndexes=*/ [],
);

st.stop();
