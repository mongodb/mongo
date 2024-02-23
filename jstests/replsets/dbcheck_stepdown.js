/**
 * Tests stepdown while dbcheck is running.
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkHealthLog,
    clearHealthLog,
    dbCheckCompleted,
    logEveryBatch,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

logEveryBatch(rst);
const dbName = "dbCheck_stepdown";
const collName = "coll";
let primary = rst.getPrimary();
let testDB = primary.getDB(dbName);
assert.commandWorked(testDB.runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: "a_1"}],
}));

// Insert nDocs, each slightly larger than the maxDbCheckMBperSec value (1MB), which is the
// default value, while maxBatchTimeMillis is defaulted to 1 second. Consequently, we will
// have only 1MB per batch and each batch will take at least 1 second on primary.
const nDocs = 5;
const chars = ['a', 'b', 'c', 'd', 'e'];
testDB[collName].insertMany(
    [...Array(nDocs).keys()].map(x => ({a: chars[x].repeat(1024 * 1024 * 2)})), {ordered: false});
rst.awaitReplication();
Random.setRandomSeed();

const stepdownWarningQuery = {
    severity: "warning",
    "msg": "abandoning dbCheck batch due to stepdown."
};
const dbCheckStartQuery = {
    severity: "info",
    operation: "dbCheckStart"
};
const dbCheckStopQuery = {
    severity: "info",
    operation: "dbCheckStop"
};

const runTest = (parameters) => {
    jsTestLog("Running dbcheck with " + tojson(parameters));
    primary = rst.getPrimary();
    testDB = primary.getDB(dbName);
    const nodeId = rst.getNodeId(primary);
    clearHealthLog(rst);
    const dbcheckFp = configureFailPoint(primary, "hangBeforeProcessingDbCheckRun");
    runDbCheck(rst, testDB, collName, parameters);
    // Make sure that dbcheck job starts.
    dbcheckFp.wait();
    dbcheckFp.off();

    // Introduce sleep intervals to randomize the timing of the stepdown while the dbcheck is
    // running, ranging between 0 and 2 seconds. This allows for the possibility of a stepdown
    // occurring anytime between the start of the dbcheck and before running the 3rd batch.
    sleep(Random.randInt(2 * 1000));
    // Step down the primary.
    assert.commandWorked(primary.getDB("admin").runCommand({replSetStepDown: 0, force: true}));

    // Wait for the cluster to come up.
    rst.awaitSecondaryNodes();

    // Find the node we ran dbCheck on.
    const node = rst.getSecondaries().filter(function isPreviousPrimary(node) {
        return rst.getNodeId(node) === nodeId;
    })[0];
    const db = node.getDB(dbName);

    // Check that it's still responding.
    try {
        assert.commandWorked(db.runCommand({ping: 1}), "ping failed after stepdown during dbCheck");
    } catch (e) {
        doassert("cannot connect after dbCheck with stepdown");
    }

    // And that our dbCheck completed.
    assert(dbCheckCompleted(db), "dbCheck failed to terminate on stepdown");
    const healthlog = node.getDB('local').system.healthlog;
    // Test health log has the expected logs after the stepdown.
    checkHealthLog(healthlog, dbCheckStartQuery, 1);
    checkHealthLog(healthlog, stepdownWarningQuery, 1);
    checkHealthLog(healthlog, dbCheckStopQuery, 1);
};

const dbCheckParameters = [
    {validateMode: "dataConsistency", maxDocsPerBatch: 1, maxBatchTimeMillis: 1000},
    {
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        maxDocsPerBatch: 1,
        bsonValidateMode: "kFull",
        maxBatchTimeMillis: 1000
    },
    {
        validateMode: "extraIndexKeysCheck",
        maxDocsPerBatch: 1,
        secondaryIndex: "a_1",
        maxBatchTimeMillis: 1000
    },
];
// Execute the test multiple times to assess the randomization of when stepdown occurs while dbcheck
// is running.
[...Array(5).keys()].map(_ => dbCheckParameters.forEach(parameters => runTest(parameters)));

rst.stopSet();
