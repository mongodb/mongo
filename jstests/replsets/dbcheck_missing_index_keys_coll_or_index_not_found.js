/**
 * Tests the dbCheck command's missing index keys check behavior when the index is not found.
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    awaitDbCheckCompletion,
    checkHealthLog,
    clearHealthLog,
    insertDocsWithMissingIndexKeys,
    logQueries,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

(function() {
"use strict";

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;

const dbName = "dbCheckMissingIndexKeys";
const collName = "dbCheckMissingIndexKeysColl";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {
        setParameter:
            {logComponentVerbosity: tojson({command: 3}), dbCheckHealthLogEveryNBatches: 1},
    }
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthLog = primary.getDB("local").system.healthlog;
const secondaryHealthLog = secondary.getDB("local").system.healthlog;
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);
assert.commandWorked(primaryDB.createCollection(collName));
const writeConcern = {
    w: 'majority'
};

const doc = {
    a: 1
};

function runIndexBuild(dbName, collName, indexSpec) {
    jsTest.log("Index build request starting...");
    db.getSiblingDB(dbName).runCommand({createIndexes: collName, indexes: [indexSpec]});
}

function dbCheckDuringIndexBuild(docSuffix) {
    jsTestLog(
        "Testing that dbcheck will not error when started while index is building and that the index will be checked after the index build completes.");

    const indexBuildFailPoint = configureFailPoint(primaryDB, "hangAfterSettingUpIndexBuild");

    // This failpoint is after a batch is completed but before it is added to the oplog. With this
    // failpoint set, dbcheck will complete 2 batches while the index is still being built.
    // After the index build completes, the remaining batches will should check the index.
    const hangBeforeAddingDBCheckBatchToOplog = configureFailPoint(
        primaryDB, "hangBeforeAddingDBCheckBatchToOplog", {} /*data*/, {"skip": 1});

    resetAndInsert(replSet, primaryDB, collName, 10, docSuffix);

    let joinIndexBuild;
    try {
        joinIndexBuild = startParallelShell(
            funWithArgs(runIndexBuild, dbName, collName, {key: {a: 1}, name: "a_1"}), primary.port);
        indexBuildFailPoint.wait();

        runDbCheck(replSet, primaryDB, collName, {
            validateMode: "dataConsistencyAndMissingIndexKeysCheck",
            maxDocsPerBatch: 1,
            batchWriteConcern: writeConcern
        });
        hangBeforeAddingDBCheckBatchToOplog.wait();
    } finally {
        indexBuildFailPoint.off();
    }

    joinIndexBuild();
    hangBeforeAddingDBCheckBatchToOplog.off();
    awaitDbCheckCompletion(replSet, primaryDB);

    // Check that the first two batches (when the index is still building) only has a count of 1
    // (count = keysSeen + docsSeen).
    checkHealthLog(primaryHealthLog, {...logQueries.infoBatchQuery, "data.count": 1}, 2);
    checkHealthLog(secondaryHealthLog, {...logQueries.infoBatchQuery, "data.count": 1}, 2);

    // Check that the rest of the batches (after the index build finishes) has a count of 2.
    checkHealthLog(primaryHealthLog, {...logQueries.infoBatchQuery, "data.count": 2}, 8);
    checkHealthLog(secondaryHealthLog, {...logQueries.infoBatchQuery, "data.count": 2}, 8);

    // Check that there are no warning/error health logs.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
}

function indexDropAfterFirstBatch() {
    jsTestLog(
        "Testing that dbcheck will not error when index is dropped in the middle of a dbcheck run.");

    clearHealthLog(replSet);
    primaryDB[collName].drop();

    // Create indexes and insert docs without inserting corresponding index keys.
    insertDocsWithMissingIndexKeys(replSet, dbName, collName, doc, 10 /*numDocs*/);

    // This failpoint is after a batch is completed but before it is added to the oplog. So with
    // this failpoint set, dbcheck will complete 2 batches while the index is present (once when
    // failpoint is off and once when the failpoint is turned on). The index will be dropped after
    // that, so any batches after the first 2 should be consistent.
    const hangBeforeAddingDBCheckBatchToOplog = configureFailPoint(
        primaryDB, "hangBeforeAddingDBCheckBatchToOplog", {} /*data*/, {"skip": 1});

    let dbCheckParameters = {
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        maxDocsPerBatch: 1,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeAddingDBCheckBatchToOplog.wait();

    assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: "a_1"}));

    hangBeforeAddingDBCheckBatchToOplog.off();
    awaitDbCheckCompletion(replSet, primaryDB);

    // Check that the primary logged 2 missing index keys query for the first two batches before the
    // index is dropped.
    checkHealthLog(primaryHealthLog, logQueries.missingIndexKeysQuery, 2);
    // Check that there are no other errors/warnings on the primary.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 2);

    // Check that the secondary logged 2 missing index keys query for the first two batches before
    // the index is dropped.
    checkHealthLog(secondaryHealthLog, logQueries.missingIndexKeysQuery, 2);
    // Check that there are no other errors/warnings on the primary.
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 2);
}

// Test with integer index entries (1, 2, 3, etc.), single character string entries ("1",
// "2", "3", etc.), and long string entries ("1aaaaaaaaaa")
[null,
 "",
 "aaaaaaaaaa"]
    .forEach((docSuffix) => {
        dbCheckDuringIndexBuild(docSuffix);
    });

indexDropAfterFirstBatch();

replSet.stopSet(undefined /* signal */,
                false /* forRestart */,
                {skipCheckDBHashes: true, skipValidation: true});
})();
