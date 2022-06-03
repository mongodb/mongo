/**
 * Test that the data tables for an incomplete index on unclean shutdown is dropped immediately as
 * opposed to deferred per the usual two-phase index drop logic.
 *
 * @tags: [
 *   requires_replication,
 *   # The primary is restarted and must retain its data.
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({name: jsTest.name(), nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "testDB";
const collName = "fantasticalCollName";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

assert.commandWorked(primaryDB.createCollection(collName));
assert.commandWorked(primaryColl.insert({x: 1}));
assert.commandWorked(primaryColl.insert({x: 2}));
assert.commandWorked(primaryColl.insert({x: 3}));

const failPoint = configureFailPoint(primaryDB, "hangIndexBuildBeforeCommit");
let ps = undefined;
try {
    TestData.dbName = dbName;
    TestData.collName = collName;
    TestData.indexSpec = {x: 1};
    TestData.indexName = {name: "myFantasticalIndex"};

    ps = startParallelShell(() => {
        jsTestLog("Starting an index build that will hang, establishing an unfinished index " +
                  "to be found on startup.");
        // Crashing the server while this command is running may cause the parallel shell code to
        // error and stop executing. We will therefore ignore the result of this command and
        // parallel shell. Test correctness is guaranteed by waiting for the failpoint this command
        // hits.
        db.getSiblingDB(TestData.dbName)[TestData.collName].createIndex(TestData.indexSpec,
                                                                        TestData.indexName);
    }, primary.port);

    jsTest.log("Waiting for the async index build to hit the failpoint.");
    failPoint.wait();

    jsTest.log(
        "Force a checkpoint, now that the index is present in the catalog. This ensures that on " +
        "startup the index will not be an 'unknown ident' discarded immediately because it does " +
        "not have an associcated catalog entry. Or some other unexpected checkpoint situation.");
    assert.commandWorked(primary.adminCommand({fsync: 1}));

    // Crash and restart the primary, which should cause the old index data to be deleted before
    // rebuilding the index from scratch.
    rst.stop(primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
} catch (error) {
    // Turn off the failpoint before allowing the test to end, so nothing hangs while the server
    // shuts down or in post-test hooks.
    failPoint.off();
    throw error;
} finally {
    if (ps) {
        ps({checkExitSuccess: false});
    }
}

// Restart the node and set 'noCleanData' so that the node's data doesn't get wiped out.
rst.start(primary, {noCleanData: true});

// Wait for the node to become available.
rst.getPrimary();

// For debugging purposes, fetch and log the test collection's indexes after restart.
const indexes =
    assert.commandWorked(rst.getPrimary().getDB(dbName).runCommand({listIndexes: collName}))
        .cursor.firstBatch;
jsTestLog("The node restarted with the following indexes on coll '" + collName +
          "': " + tojson(indexes));

// Now that the node is up and running, check that the expected code path was hit, to immediately
// drop the index data table on startup.
checkLog.containsJson(rst.getPrimary(), 6361201, {index: "myFantasticalIndex"});

rst.stopSet();
})();
