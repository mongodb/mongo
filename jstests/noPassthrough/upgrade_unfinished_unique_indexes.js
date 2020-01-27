/**
 * Starts a replica set, builds an index and shuts down the secondary. During startup recovery the
 * FCV is uninitialized, and index is built in a lower format version. Expect that the unfinished
 * index is correctly upgraded before startup is complete. See SERVER-45374.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/check_log.js');

const dbname = 'bgIndexSec';
const collection = 'bgIndexShutdown';

// Set up replica set
const replTest = new ReplSetTest({
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
const nodes = replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const secondaryId = replTest.getNodeId(secondary);

const primaryDB = primary.getDB(dbname);
const secondDB = secondary.getDB(dbname);

primaryDB.dropDatabase();
const primaryColl = primaryDB.getCollection(collection);
assert.commandWorked(primaryColl.insert({a: 1}));

// Stop the index build before it completes so the server can shut down while it is unfinished.
IndexBuildTest.pauseIndexBuilds(secondary);

const indexName = "a_1";
assert.commandWorked(primaryDB.runCommand({
    createIndexes: collection,
    indexes: [{key: {a: 1}, name: indexName, unique: true}],
}));
const indexes = primaryColl.getIndexes();
assert.eq(2, indexes.length, tojson(indexes));

// Wait for index builds to start on the secondary.
IndexBuildTest.waitForIndexBuildToStart(secondDB);

jsTest.log("Restarting secondary and waiting for the index to be rebuilt");

// Secondary should restart cleanly.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'leaveIndexBuildUnfinishedForShutdown', mode: 'alwaysOn'}));
IndexBuildTest.resumeIndexBuilds(secondary);
replTest.restart(secondaryId, {}, /*wait=*/true);

// There should be a message for the index that was interrupted. The server restarts the index build
// on startup, and additionally upgrades the metadata for the unique index.
checkLog.containsWithCount(
    replTest.getSecondary(),
    `Upgrading unique index metadata for '${indexName}' on collection ${primaryColl.getFullName()}`,
    1);

replTest.stopSet();
}());
