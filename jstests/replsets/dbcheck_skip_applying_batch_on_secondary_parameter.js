/**
 * Tests the skipApplyingDbCheckBatchOnSecondary parameter.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    checkHealthLog,
    clearHealthLog,
    logQueries,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

const dbName = jsTestName();
const collName = jsTestName();

const nDocs = 20;
const batchSize = 4;

function runTest(validateMode) {
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

    assert.commandWorked(
        secondary.adminCommand({"setParameter": 1, "skipApplyingDbCheckBatchOnSecondary": true}));
    const writeConcern = {w: 'majority'};

    resetAndInsert(rst, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    rst.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);
    clearHealthLog(rst);

    if (validateMode == "dataConsistencyCheck") {
        jsTestLog("Running dbCheck dataConsistencyCheck");
        runDbCheck(rst,
                   primary.getDB(dbName),
                   collName,
                   {
                       maxDocsPerBatch: batchSize,
                       batchWriteConcern: writeConcern,
                   },
                   true /*awaitCompletion*/);
    } else if (validateMode == "extraIndexKeysCheck") {
        jsTestLog("Running dbCheck extraIndexKeysCheck");
        runDbCheck(rst,
                   primary.getDB(dbName),
                   collName,
                   {
                       validateMode: "extraIndexKeysCheck",
                       secondaryIndex: "a_1",
                       maxDocsPerBatch: batchSize,
                       batchWriteConcern: writeConcern,
                   },
                   true /*awaitCompletion*/);
    }

    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, nDocs / batchSize);

    checkHealthLog(secondaryHealthLog, logQueries.startStopQuery, 2);
    checkHealthLog(
        secondaryHealthLog, logQueries.skipApplyingBatchOnSecondaryQuery, nDocs / batchSize);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs / batchSize);
    checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 0);
    rst.stopSet();
}

["extraIndexKeysCheck",
 "dataConsistencyCheck",
].forEach((validateMode) => {
    runTest(validateMode);
});
