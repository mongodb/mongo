/**
 * Tests the demoMode parameter for the reshardCollection command.
 * When demoMode is true, the resharding operation should complete quickly on a small dataset as
 * reshardingMinimumOperationDurationMillis and
 * reshardingDelayBeforeRemainingOperationTimeQueryMillis are both set to 0.
 *
 * @tags: [
 * requires_fcv_82,
 * # This test performs explicit calls to shardCollection
 * assumes_unsharded_collection,
 * # This test does not assert specific chunk placements.
 * assumes_balancer_off,
 * # To avoid retrying logic in the test.
 * does_not_support_stepdowns,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";
import {getShardNames} from "jstests/sharding/libs/sharding_util.js";

// This test requires at least two shards.
const shardNames = getShardNames(db);
if (shardNames.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const topology = DiscoverTopology.findConnectedNodes(db);
const configPrimary = new Mongo(topology.configsvr.primary);
const reshardingDelayBeforeRemainingOperationTimeQueryMillisValue = 20000;
// Explicitly set the resharding params to maintain test duration invariants.
// Note that we only set and test reshardingDelayBeforeRemainingOperationTimeQueryMillisValue below
// mainly as reshardingMinimumOperationDurationMillis is covered in CPP tests.
const setParamCmd = Object.assign({setParameter: 1}, {
    reshardingMinimumOperationDurationMillis: 0,
    reshardingDelayBeforeRemainingOperationTimeQueryMillis:
        reshardingDelayBeforeRemainingOperationTimeQueryMillisValue
});
assert.commandWorked(configPrimary.adminCommand(setParamCmd));

const collName = jsTestName();
const dbName = db.getName();
const ns = dbName + '.' + collName;
const mongos = db.getMongo();
const kNumInitialDocs = 10;
const reshardCmdTest = new ReshardCollectionCmdTest({
    mongos,
    dbName: dbName,
    collName,
    numInitialDocs: kNumInitialDocs,
    skipDirectShardChecks: true
});

const testDemoModeTrue = (mongos) => {
    jsTestLog("Testing reshardCollection with demoMode: true should complete quickly.");

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
    const maxExpectedDurationMs = reshardingDelayBeforeRemainingOperationTimeQueryMillisValue - 1;
    reshardCmdTest.assertReshardCollOk(
        {reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1, demoMode: true},
        1);  // Expect 1 initial chunk

    assert.lte(
        reshardCmdTest._reshardDuration,
        maxExpectedDurationMs,
        `Resharding with demoMode=true took ${
            reshardCmdTest._reshardDuration}ms, which is longer than the expected maximum of ${
            maxExpectedDurationMs}ms.`);
};

const testDemoModeFalse = (mongos) => {
    jsTestLog(
        "Testing reshardCollection with demoMode: false should take at least reshardingDelayBeforeRemainingOperationTimeQueryMillis" +
        reshardingDelayBeforeRemainingOperationTimeQueryMillisValue + " seconds to complete");

    // Drop the collection to ensure a clean state for this test
    mongos.getDB(dbName)[collName].drop();
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

    const startTime = Date.now();
    reshardCmdTest.assertReshardCollOk(
        {reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1, demoMode: false},
        1);  // Expect 1 initial chunk

    const endTime = Date.now();
    const durationMs = endTime - startTime;
    const minExpectedDurationMs = reshardingDelayBeforeRemainingOperationTimeQueryMillisValue;

    assert.gte(durationMs,
               minExpectedDurationMs,
               `Resharding with demoMode=false took ${
                   durationMs}ms, which is shorter than the expected minimum of ${
                   minExpectedDurationMs}ms.`);
};

testDemoModeTrue(mongos);
testDemoModeFalse(mongos);
