/**
 * Starts a replica set with arbiter, builds an index in background.
 * Kills the secondary once the index build starts with a failpoint.
 * The index build should resume when the secondary is restarted.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_journaling,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

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

// We need an arbiter to ensure that the primary doesn't step down
// when we restart the secondary.
const nodes = replTest.startSet();
replTest.initiate();

var primary = replTest.getPrimary();
var second = replTest.getSecondary();

var primaryDB = primary.getDB('bgIndexSec');
var secondDB = second.getDB('bgIndexSec');

var collectionName = 'jstests_bgsec';

var coll = primaryDB.getCollection(collectionName);

var size = 100;

var bulk = primaryDB.jstests_bgsec.initializeUnorderedBulkOp();
for (var i = 0; i < size; ++i) {
    bulk.insert({i: i, x: i, y: i});
}
assert.commandWorked(bulk.execute({j: true}));
assert.eq(size, coll.count(), 'unexpected number of documents after bulk insert.');

// Make sure the documents make it to the secondary.
replTest.awaitReplication();

// Pause the index build on the primary after replicating the startIndexBuild oplog entry.
IndexBuildTest.pauseIndexBuilds(primaryDB);
IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});
IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});
IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {y: 1});

// Wait for build to start on the secondary.
jsTestLog("waiting for all index builds to start on secondary");
IndexBuildTest.waitForIndexBuildToStart(secondDB, coll.getName(), "y_1");

MongoRunner.stopMongod(second);

replTest.start(second,
               {
                   setParameter: {
                       "failpoint.hangAfterSettingUpIndexBuildUnlocked": tojson({mode: "alwaysOn"}),
                       "failpoint.hangAfterSettingUpResumableIndexBuild": tojson({mode: "alwaysOn"})
                   }
               },
               /*restart=*/true,
               /*wait=*/true);

// Make sure secondary comes back.
try {
    assert.soon(function() {
        try {
            secondDB.isMaster();  // trigger a reconnect if needed
            return true;
        } catch (e) {
            return false;
        }
    }, "secondary didn't restart", 30000, 1000);

    // Verify that we do not wait for the index build to complete on startup.
    assert.eq(size, secondDB.getCollection(collectionName).find({}).itcount());

    // Verify that only the _id index is ready.
    if (ResumableIndexBuildTest.resumableIndexBuildsEnabled(second)) {
        checkLog.containsJson(second, 4841704);
    } else {
        checkLog.containsJson(second, 4585201);
    }
    IndexBuildTest.assertIndexes(secondDB.getCollection(collectionName),
                                 4,
                                 ["_id_"],
                                 ["i_1", "x_1", "y_1"],
                                 {includeBuildUUIDS: true});
} finally {
    assert.commandWorked(second.adminCommand(
        {configureFailPoint: 'hangAfterSettingUpIndexBuildUnlocked', mode: 'off'}));
    assert.commandWorked(second.adminCommand(
        {configureFailPoint: 'hangAfterSettingUpResumableIndexBuild', mode: 'off'}));

    // Let index build complete on primary, which replicates a commitIndexBuild to the secondary.
    IndexBuildTest.resumeIndexBuilds(primaryDB);
}

assert.soon(function() {
    return 4 == secondDB.getCollection(collectionName).getIndexes().length;
}, "Index build not resumed after restart", 30000, 50);
replTest.stopSet();
}());
