/*
 * Test that startup recovery oplog application skips dbcheck oplog entries during the manual
 * restore procedure.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_wiredtiger,
 *     requires_replication,
 *     featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {copyFileHelper, openBackupCursor} from "jstests/libs/backup_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    checkHealthLog,
    insertDocsWithMissingIndexKeys,
    logEveryBatch,
    logQueries,
    runDbCheck,
} from "jstests/replsets/libs/dbcheck_utils.js";

// TODO SERVER-87225: Enable fast count on validate when operations applied during a restore are
// counted correctly.
TestData.skipEnforceFastCountOnValidate = true;
// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;
TestData.skipCollectionAndIndexValidation = true;

const rst = new ReplSetTest({
    nodes: [{}, {}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    settings: {chainingAllowed: false}
});

const nodes = rst.startSet();
let restoreNode = nodes[1];
rst.initiate();
logEveryBatch(rst);

const primary = rst.getPrimary();
const dbName = "testDB";
const db = primary.getDB(dbName);
const collName = "testcoll";
const nDocs = 10;

// Opening backup cursors can race with taking a checkpoint, so disable checkpoints.
// This makes testing quicker and more predictable. In production, a poorly interleaved checkpoint
// will return an error, requiring retry.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "alwaysOn"}));

// Insert some documents with missing index keys on the restore node.
insertDocsWithMissingIndexKeys(
    rst, dbName, collName, {a: 1}, nDocs, false /* doPrimary */, true /* doSecondary */);
rst.awaitReplication();

// Take the checkpoint to be used by the backup cursor. Operations done beyond this point will be
// replayed from the oplog during startup recovery.
assert.commandWorked(db.adminCommand({fsync: 1}));

// Start dbcheck after the checkpoint is taken so that dbcheck oplogs will be replayed (ignored).
const dbCheckParameters = {
    validateMode: "dataConsistencyAndMissingIndexKeysCheck",
    maxDocsPerBatch: 100,
    batchWriteConcern: {w: 4}
};
jsTestLog(
    "Testing startup recovery for restore oplog application with dbcheck, to ensure dbcheck oplog entries are correctly skipped during oplog application.");
runDbCheck(rst, db, collName, dbCheckParameters, true /*awaitCompletion*/);

// Check that the restore node logged an error health log entry for each document with missing index
// key.
let restoreNodeHealthLog = restoreNode.getDB("local").system.healthlog;
checkHealthLog(restoreNodeHealthLog, logQueries.missingIndexKeysQuery, nDocs);
// Check that there are no other errors/warnings on the restore node.
checkHealthLog(restoreNodeHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs);
checkHealthLog(restoreNodeHealthLog, logQueries.startStopQuery, 2);

// sleep(2000);

rst.awaitNodesAgreeOnAppliedOpTime();

const backupDbPath = primary.dbpath + "/backup";
resetDbpath(backupDbPath);
mkdir(backupDbPath + "/journal");

// Open a backup cursor on the checkpoint.
let backupCursor = openBackupCursor(primary.getDB("admin"));

// Print the metadata document.
assert(backupCursor.hasNext());
jsTestLog("Backup cursor metadata document: " + tojson(backupCursor.next()));

while (backupCursor.hasNext()) {
    let doc = backupCursor.next();
    jsTestLog("Copying for backup: " + tojson(doc));
    copyFileHelper({filename: doc.filename, fileSize: doc.fileSize}, primary.dbpath, backupDbPath);
}

backupCursor.close();

assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "off"}));

rst.stopSet(/*signal=*/ null, /*forRestart=*/ true);

// Startup with '--recoverFromOplogAsStandalone'.
restoreNode = MongoRunner.runMongod({
    dbpath: backupDbPath,
    noCleanData: true,
    setParameter: {
        startupRecoveryForRestore: true,
        recoverFromOplogAsStandalone: true,
        takeUnstableCheckpointOnShutdown: true
    }
});
assert(restoreNode);
restoreNodeHealthLog = restoreNode.getDB("local").system.healthlog;

// Check that the restore node ignored dbcheck oplogs during backup restore. There should be
// 1 start, 1 batch, and 1 stop.
checkHealthLog(restoreNodeHealthLog, logQueries.duringStableRecovery, 3);
// Check that there are no other health log entries because dbcheck started after the checkpoint is
// taken.
checkHealthLog(restoreNodeHealthLog, logQueries.allErrorsOrWarningsQuery, 3);

MongoRunner.stopMongod(restoreNode);
