/**
 * Tests that "getMore" command on a change stream cursor returns an error classified as resumable
 * change stream error upon primary node rollback which is a member of a replica set or a config
 * replica set of the sharded cluster.
 *
 * @tags: [
 *  requires_replication,
 *  requires_sharding,
 *  requires_mongobridge,
 * ]
 */
import {
    restartReplSetReplication,
} from "jstests/libs/write_concern_util.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

// Verifies that "getMore" command on a change stream cursor returns an error classified as
// resumable change stream error upon primary node rollback. 'rollbackTest' is an instance of
// 'RollbackTest' representing a replica set to perform the test on.
function verifyChangeStreamReturnsResumableChangeStreamErrorOnNodeRollback(rollbackTest) {
    assert(rollbackTest instanceof RollbackTest);
    const primaryNodeConnection = rollbackTest.getPrimary();
    const testDB = primaryNodeConnection.getDB('test');
    const collectionName = "coll";
    assert.commandWorked(testDB.createCollection(collectionName));
    const collection = testDB[collectionName];

    // Open a change stream.
    const changeStreamCursor = collection.watch([]);

    // Perform operations that will be rolled back.
    rollbackTest.transitionToRollbackOperations();
    assert.commandWorked(collection.insert({doc: 1}));

    // Perform the rollback on the current primary node.
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    // Verify that when "getMore" command is issued, the server returns an error response with error
    // code 'ErrorCodes.QueryPlanKilled' and "ResumableChangeStreamError" label.
    const response = assert.throwsWithCode(() => {
        changeStreamCursor.hasNext();
    }, ErrorCodes.QueryPlanKilled, []);
    assert(response.hasOwnProperty("errorLabels") &&
               response.errorLabels.includes("ResumableChangeStreamError"),
           `Expected "ResumableChangeStreamError" label in the "getMore" command response: ${
               tojson(response)}`);
}

// Perform a test on a replica set.
const replicaSetRollbackTest = new RollbackTest(jsTestName());
verifyChangeStreamReturnsResumableChangeStreamErrorOnNodeRollback(replicaSetRollbackTest);
replicaSetRollbackTest.stop();

// Perform a test on the config replica set of the sharded cluster. Note that a special setup of the
// sharded cluster is required for the RollbackTest to work.
const shardingTest = new ShardingTest({
    shards: 1,
    mongos: 1,
    configReplSetTestOptions:
        {settings: {chainingAllowed: false, electionTimeoutMillis: ReplSetTest.kForeverMillis}},
    config: [{}, {}, {rsConfig: {priority: 0}}],
    other: {useBridge: true},
});
const configReplicaSetRollbackTest = new RollbackTest(jsTestName(), shardingTest.configRS);
verifyChangeStreamReturnsResumableChangeStreamErrorOnNodeRollback(configReplicaSetRollbackTest);

// Restart the replication in the config replica set so the tear down of the sharded cluster can
// happen.
restartReplSetReplication(shardingTest.configRS);
shardingTest.stop();
