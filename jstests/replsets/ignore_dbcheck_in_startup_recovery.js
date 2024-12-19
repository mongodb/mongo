/**
 * Test that dbcheck command will be ignored during startup recovery.
 *
 * @tags: [
 *   requires_fcv_81,
 *   # Requires persistency because we restart a node to enter startup recovery.
 *   requires_persistence,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    checkHealthLog,
    logQueries,
    resetAndInsert,
    runDbCheck
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
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}}
});
replSet.startSet();
replSet.initiate();

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

runDbCheck(replSet,
           primaryDb,
           collName,
           {
               validateMode: "extraIndexKeysCheck",
               secondaryIndex: "a_1",
               maxDocsPerBatch: 20,
               batchWriteConcern: {w: 2}
           },
           true /*awaitCompletion*/);

// Wait to make sure that the secondary applied the stop entry before the ungraceful shutdown.
replSet.awaitNodesAgreeOnAppliedOpTime();

// Perform ungraceful shutdown of the secondary node and do not clean the db path directory.
replSet.stop(
    1, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true, skipValidation: true});

secondary = replSet.start(secondary,
                          {
                              setParameter: {
                                  "failpoint.stopReplProducer": tojson({"mode": "alwaysOn"}),
                              },
                          },
                          {noCleanData: true});

primary = replSet.getPrimary();

const primaryHealthLog = primary.getDB("local").system.healthlog;
const secondaryHealthLog = secondary.getDB("local").system.healthlog;

// Check that the start, batch, and stop entries are warning logs since the node skips dbcheck
// during startup recovery.
checkHealthLog(secondaryHealthLog, logQueries.duringStableRecovery, 12);
// Check that the initial sync node does not have other info or error/warning entries.
checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 0);
checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 12);

// Check that the primary logged an error health log entry for each document with missing index key.
checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocs);
// Check that the primary does not have other error/warning entries.
checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs);

skipUnindexingDocumentWhenDeleted.off();
replSet.stopSet();
