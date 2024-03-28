/**
 * Tests dbCheck health log entries are consistent for all nodes in the replica sets.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {checkHealthLog, resetAndInsert, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

const nDocs = 10;
const dbCheckHealthLogEveryNBatches = 4;
// We are going to force 1 doc per batch.
const numBatchLogExpected = Math.floor(nDocs / dbCheckHealthLogEveryNBatches);
const maxBatchTimeMillis = 10;

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: dbCheckHealthLogEveryNBatches}},
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const dbName = "dbCheckConsistentHealthLog";
const collName = "collName";
const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthlog = primary.getDB("local").system.healthlog;
const secondaryHealthlog = secondary.getDB("local").system.healthlog;
const db = primary.getDB(dbName);

// Only run this test for debug=off, because we log all batches in debug builds.
const debugBuild = db.adminCommand("buildInfo").debug;
if (debugBuild) {
    jsTestLog("Skipping the test because debug is on.");
} else {
    // That will force the batch to only have 1 document as the hasher deadline will expire after
    // reading one document.
    configureFailPoint(primary, 'SleepDbCheckInBatch', {sleepMs: maxBatchTimeMillis * 2});

    function healthLogConsistent() {
        jsTestLog("Clear healthLog and run dbcheck and waits for it to finish.");
        resetAndInsert(replSet, db, collName, nDocs);
        runDbCheck(replSet,
                   db,
                   collName,
                   {maxBatchTimeMillis: maxBatchTimeMillis},
                   true /* awaitCompletion */);

        const collUUID = db.getCollectionInfos({name: collName})[0].info.uuid;
        let query = {operation: "dbCheckBatch", collectionUUID: collUUID};
        checkHealthLog(primaryHealthlog, query, numBatchLogExpected);
        checkHealthLog(secondaryHealthlog, query, numBatchLogExpected);

        jsTestLog("Testing that 'data.batchId' field exist in batch health logs.");
        // There are no dbCheckBatch health log entries without a batchId.
        query = {operation: "dbCheckBatch", "data.batchId": {$exists: false}};
        checkHealthLog(primaryHealthlog, query, 0);
        checkHealthLog(secondaryHealthlog, query, 0);

        let primaryHealthLogs =
            primaryHealthlog.find({operation: "dbCheckBatch"}).toArray().reduce((map, log) => {
                map[log.data.batchId] = log.data;
                return map;
            }, {});
        let secondaryHealthLogs =
            secondaryHealthlog.find({operation: "dbCheckBatch"}).toArray().reduce((map, log) => {
                map[log.data.batchId] = log.data;
                return map;
            }, {});

        jsTestLog("Batch healthlog entries should be the same across nodes.");
        assert.eq(tojson(primaryHealthLogs), tojson(secondaryHealthLogs));
    }

    // Running the dbcheck multiple times should invalidate all the in-memory states between primary
    // and secondaries. That should be false if the secondaries is keeping a global number of
    // batches while the primary keeps a local number for each dbcheck run because (NumberOfBatches
    // % dbCheckHealthLogEveryNBatches (10 % 4)) != 0.
    for (var i = 0; i < 3; i++) {
        healthLogConsistent();
    }
}
replSet.stopSet();
