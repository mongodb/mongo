/**
 * Tests dbCheck fails on write concern error.
 *
 * @tags: [
 *   requires_fcv_81
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    restartReplicationOnSecondaries,
    stopReplicationOnSecondaries
} from "jstests/libs/write_concern_util.js";
import {
    checkHealthLog,
    clearHealthLog,
    logQueries,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

const dbName = jsTestName();
const collName = jsTestName();

// There should be multiple batches in the dbcheck run, but dbcheck should stop after the first
// batch fails when waiting for write concern.
const nDocs = 100;
const batchSize = 10;

function runTest(validateMode, writeConcern) {
    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: 2,
        nodeOptions: {
            setParameter:
                {logComponentVerbosity: tojson({command: 3}), dbCheckHealthLogEveryNBatches: 1},
        }
    });
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const primaryHealthLog = primary.getDB("local").system.healthlog;
    const secondaryHealthLog = secondary.getDB("local").system.healthlog;
    const primaryDB = primary.getDB(dbName);
    const secondaryDB = secondary.getDB(dbName);

    resetAndInsert(rst, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    rst.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);
    clearHealthLog(rst);

    const hangBeforeProcessingDbCheckRunFp =
        configureFailPoint(primary, "hangBeforeProcessingDbCheckRun");

    const hangBeforeAddingDBCheckBatchToOplogFp =
        configureFailPoint(primary, "hangBeforeAddingDBCheckBatchToOplog");

    if (validateMode == "dataConsistencyAndMissingIndexKeysCheck") {
        jsTestLog("Running dbCheck dataConsistencyAndMissingIndexKeysCheck");
        runDbCheck(rst, primary.getDB(dbName), collName, {
            validateMode: "dataConsistencyAndMissingIndexKeysCheck",
            maxDocsPerBatch: batchSize,
            batchWriteConcern: writeConcern,
        });
    } else if (validateMode == "extraIndexKeysCheck") {
        jsTestLog("Running dbCheck extraIndexKeysCheck");
        runDbCheck(rst, primary.getDB(dbName), collName, {
            validateMode: "extraIndexKeysCheck",
            secondaryIndex: "a_1",
            maxDocsPerBatch: batchSize,
            batchWriteConcern: writeConcern,
        });
    }

    hangBeforeProcessingDbCheckRunFp.wait();
    stopReplicationOnSecondaries(rst);

    hangBeforeProcessingDbCheckRunFp.off();
    hangBeforeAddingDBCheckBatchToOplogFp.wait();
    hangBeforeAddingDBCheckBatchToOplogFp.off();

    // Verify that dbCheck stopped after write concern error.
    checkHealthLog(primaryHealthLog, logQueries.writeConcernErrorQuery, 1);
    checkHealthLog(primaryHealthLog, logQueries.startStopQuery, 2);
    // 1 for start, 1 for the batch, 1 for write concern error, 1 for stop
    checkHealthLog(primaryHealthLog, {}, 4);
    checkHealthLog(secondaryHealthLog, logQueries.startStopQuery, 1);
    checkHealthLog(secondaryHealthLog, {}, 1);

    restartReplicationOnSecondaries(rst);
    rst.stopSet();
}

["dataConsistencyAndMissingIndexKeysCheck",
 "extraIndexKeysCheck"]
    .forEach((failpointName) => {
        runTest(failpointName, {
            w: 'majority',
            wtimeout: 100,
        });
        runTest(failpointName, {
            w: 3,
            wtimeout: 100,
        });
    });
