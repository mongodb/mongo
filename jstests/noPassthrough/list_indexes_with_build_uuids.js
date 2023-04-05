/**
 * Ensures that the 'buildUUID' is present for in-progress indexes when using the 'listIndexes()'
 * command.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const dbName = "test";
const collName = "coll";

const firstIndexName = "first";
const secondIndexName = "second";

function addTestDocuments(db) {
    let size = 100;
    jsTest.log("Creating " + size + " test documents.");
    var bulk = db.getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < size; ++i) {
        bulk.insert({i: i, j: i * i});
    }
    assert.commandWorked(bulk.execute());
}

const replSet = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
const nodes = replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let primaryDB = primary.getDB(dbName);

let secondary = replSet.getSecondary();
let secondaryDB = secondary.getDB(dbName);

addTestDocuments(primaryDB);
replSet.awaitReplication();

// Build and finish the first index.
assert.commandWorked(primaryDB.runCommand(
    {createIndexes: collName, indexes: [{key: {i: 1}, name: firstIndexName, background: true}]}));
replSet.awaitReplication();

// Start hanging index builds on the secondary.
IndexBuildTest.pauseIndexBuilds(secondary);

// With storage engines that do not support snapshot reads, the commitIndexBuild oplog entry may
// block the listIndexes command on the secondary during oplog application because it will hold the
// PBWM while waiting for the index build to complete in the backgroud. Therefore, we get the
// primary to hold off on writing the commitIndexBuild oplog entry until we are ready to resume
// index builds on the secondary.
IndexBuildTest.pauseIndexBuilds(primary);

// Build and hang on the second index. This should be run in the background if we pause index
// builds on the primary because the createIndexes command will block.
const coll = primaryDB.getCollection(collName);
const createIdx =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {j: 1}, {name: secondIndexName});

// Wait for index builds to start on the secondary.
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
jsTestLog('Index builds started on secondary. Op ID of one of the builds: ' + opId);

// Wait for the index build to be initialized, so it is shown in listIndexes.
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "hangAfterStartingIndexBuild",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Check the listIndexes() output.
let res = secondaryDB.runCommand({listIndexes: collName, includeBuildUUIDs: true});

assert.commandWorked(res);
let indexes = res.cursor.firstBatch;
assert.eq(3, indexes.length, tojson(res));

jsTest.log(indexes);

assert.eq(indexes[0].name, "_id_");
assert.eq(indexes[1].name, "first");

assert.eq(indexes[2].spec.name, "second");
assert(indexes[2].hasOwnProperty("buildUUID"));

// Allow the replica set to finish the index build.
IndexBuildTest.resumeIndexBuilds(secondary);

// Wait for the index build to complete on the primary if we paused it.
IndexBuildTest.resumeIndexBuilds(primary);
createIdx();

replSet.stopSet();
}());
