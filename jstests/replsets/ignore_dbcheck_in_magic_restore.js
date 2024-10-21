/*
 * Test that startup recovery oplog application skips dbcheck oplog entries during magic restore.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_wiredtiger,
 *     featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {_copyFileHelper, MagicRestoreUtils} from "jstests/libs/backup_utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    checkHealthLog,
    logEveryBatch,
    logQueries,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// TODO SERVER-86034: Run on Windows machines once named pipe related failures are resolved.
if (_isWindows()) {
    jsTestLog("Temporarily skipping test for Windows variants. See SERVER-86034.");
    quit();
}

// TODO SERVER-87225: Enable fast count on validate when operations applied during a restore are
// counted correctly.
TestData.skipEnforceFastCountOnValidate = true;
// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;
TestData.skipCollectionAndIndexValidation = true;

const sourceCluster = new ReplSetTest({nodes: 1});
sourceCluster.startSet();
sourceCluster.initiate();
logEveryBatch(sourceCluster);

const sourcePrimary = sourceCluster.getPrimary();
const dbName = "db";
const coll = "coll";
const nDocs = 10;

const sourceDb = sourcePrimary.getDB(dbName);
const sourceColl = sourceDb.getCollection(coll);
// Insert some data with inconsistency to restore. This data will be reflected in the restored node.
resetAndInsert(sourceCluster, sourceDb, coll, nDocs);
assert.commandWorked(sourceDb.runCommand({
    createIndexes: coll,
    indexes: [{key: {a: 1}, name: 'a_1'}],
}));
assert.eq(sourceColl.find({}).count(), nDocs);
const skipUnindexingDocumentWhenDeletedPrimary =
    configureFailPoint(sourceDb, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
jsTestLog("Deleting docs");
assert.commandWorked(sourceColl.deleteMany({}));
assert.eq(sourceColl.find({}).count(), 0);

const magicRestoreUtils = new MagicRestoreUtils(
    {backupSource: sourcePrimary, pipeDir: MongoRunner.dataDir, insertHigherTermOplogEntry: true});

magicRestoreUtils.takeCheckpointAndOpenBackup();

// Start dbcheck after the backup cursor was opened. The dbcheck oplog entries will be passed to
// magic restore to perform a PIT restore.
const dbCheckParameters = {
    validateMode: "extraIndexKeysCheck",
    secondaryIndex: "a_1",
    maxDocsPerBatch: 10,
    batchWriteConcern: {w: 'majority'}
};
jsTestLog(
    "Testing PIT magic restore with dbcheck to ensure dbcheck oplog entries are correctly skipped during recovery oplog application.");
runDbCheck(sourceCluster, sourceDb, coll, dbCheckParameters, true /*awaitCompletion*/);

const sourcePrimaryHealthLog = sourcePrimary.getDB("local").system.healthlog;
// Check that the primary logged an error health log entry for each document with extra index key.
checkHealthLog(sourcePrimaryHealthLog, logQueries.recordNotFoundQuery, nDocs);
// Check that there are no other errors/warnings on the primary.
checkHealthLog(sourcePrimaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs);

const checkpointTimestamp = magicRestoreUtils.getCheckpointTimestamp();
let {lastOplogEntryTs, entriesAfterBackup} = magicRestoreUtils.getEntriesAfterBackup(sourcePrimary);

magicRestoreUtils.copyFilesAndCloseBackup();

let expectedConfig = assert.commandWorked(sourcePrimary.adminCommand({replSetGetConfig: 1})).config;
// The new node will be allocated a new port by the test fixture.
expectedConfig.members[0].host = getHostName() + ":" + (Number(sourcePrimary.port) + 2);
let restoreConfiguration = {
    "nodeType": "replicaSet",
    "replicaSetConfig": expectedConfig,
    "maxCheckpointTs": checkpointTimestamp,
    // Restore to the timestamp of the last oplog entry on the source cluster.
    "pointInTimeTimestamp": lastOplogEntryTs
};
restoreConfiguration =
    magicRestoreUtils.appendRestoreToHigherTermThanIfNeeded(restoreConfiguration);

magicRestoreUtils.writeObjsAndRunMagicRestore(restoreConfiguration, entriesAfterBackup, {
    "replSet": jsTestName(),
    setParameter: {"failpoint.sleepToWaitForHealthLogWrite": "{'mode': 'alwaysOn'}"}
});

// Start a new replica set fixture on the dbpath.
const destinationCluster = new ReplSetTest({nodes: 1});
destinationCluster.startSet({dbpath: magicRestoreUtils.getBackupDbPath(), noCleanData: true});

let destPrimary = destinationCluster.getPrimary();
const destPrimaryHealthLog = destPrimary.getDB("local").system.healthlog;

// Check that the start, batch, and stop entries are warning logs since the node skips dbcheck
// during startup recovery. During restore, the health log collection recovers to stableTimestamp
// (empty at that timestamp because dbcheck was run after the checkpoint was taken). When replaying
// the oplog entries during recovery, dbcheck generates these warning logs instead to skip dbcheck.
checkHealthLog(destPrimaryHealthLog, logQueries.duringStableRecovery, 3);
// Check that there are no other health log entries because dbcheck started after the checkpoint is
// taken.
checkHealthLog(destPrimaryHealthLog, logQueries.allErrorsOrWarningsQuery, 3);

sourceCluster.stopSet();
destinationCluster.stopSet();
