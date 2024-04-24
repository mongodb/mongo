/**
 * Test that dbcheck command will be ignored during startup recovery.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck, requires_persistence
 * ]
 */

load("jstests/libs/fail_point_util.js");
import {
    checkHealthLog,
    logQueries,
    resetAndInsert,
    runDbCheck,
    awaitDbCheckCompletion
} from "jstests/replsets/libs/dbcheck_utils.js";

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "ignore_dbcheck_in_startup_recovery";
const collName = "ignore_dbcheck_in_startup_recovery-collection";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {
        setParameter:
            {logComponentVerbosity: tojson({command: 3}), dbCheckHealthLogEveryNBatches: 1}
    }
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

let primary = replSet.getPrimary();
const primaryDb = primary.getDB(dbName);
let secondary = replSet.getSecondary();
const secondaryDb = secondary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

const nDocs = 200;
resetAndInsert(replSet, primaryDb, collName, nDocs);
assert.commandWorked(
    primaryDb.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}]}));
replSet.awaitReplication();
assert.eq(primaryColl.find({}).count(), nDocs);

// Set up inconsistency.
const skipUnindexingDocumentWhenDeleted =
    configureFailPoint(primaryDb, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
jsTestLog("Deleting docs");
const stableTimestamp = assert.commandWorked(primaryColl.deleteMany({}));

replSet.awaitReplication();
assert.eq(primaryColl.find({}).count(), 0);
assert.eq(secondaryDb.getCollection(collName).find({}).count(), 0);

configureFailPoint(primary, "holdStableTimestampAtSpecificTimestamp", {timestamp: stableTimestamp});

// TODO SERVER-89921: Uncomment validateMode and secondaryIndex once the relevant tickets are
// backported.
runDbCheck(replSet,
           primaryDb,
           collName,
           {
               maxDocsPerBatch: 20,
               minKey: {a: "0"},
               maxKey: {a: "199"},
               batchWriteConcern: {w: 1},
               // , validateMode: "extraIndexKeysCheck", secondaryIndex: "a_1"
           },
           true /*awaitCompletion*/);

// Perform ungraceful shutdown of the secondary node and do not clean the db path directory.
replSet.stop(
    1, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true, skipValidation: true});

secondary = replSet.start(secondary,
                          {
                              setParameter: {
                                  "failpoint.stopReplProducer": tojson({"mode": "alwaysOn"}),
                                  logComponentVerbosity: tojson({command: 3}),
                                  dbCheckHealthLogEveryNBatches: 1,
                              },
                          },
                          {noCleanData: true});

primary = replSet.getPrimary();

replSet.awaitReplication();

const primaryHealthLog = primary.getDB("local").system.healthlog;
const secondaryHealthLog = secondary.getDB("local").system.healthlog;

// Check that the start, batch, and stop entries are warning logs since the node skips dbcheck
// during startup recovery. Since dbcheck on 7.0 currently does not use `maxDocsPerBatch`, it will
// check all the data in one batch.
checkHealthLog(secondaryHealthLog, logQueries.duringStableRecovery, 3);
// Check that the initial sync node does not have other info or error/warning entries.
checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 0);
checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 3);

// TODO SERVER-89921: Uncomment checkHealthLog once the relevant tickets are backported.
// Check that the primary logged an error health log entry for each document with missing index key.
// checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocs);
// Check that the primary does not have other error/warning entries.
// checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs);

skipUnindexingDocumentWhenDeleted.off();
replSet.stopSet();
