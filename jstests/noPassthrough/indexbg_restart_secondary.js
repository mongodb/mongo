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

if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    // Pause the index build on the primary after replicating the startIndexBuild oplog entry.
    IndexBuildTest.pauseIndexBuilds(primaryDB);
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {y: 1});

    // Wait for build to start on the secondary.
    jsTestLog("waiting for index build to start on secondary");
    IndexBuildTest.waitForIndexBuildToStart(secondDB);
} else {
    assert.commandWorked(secondDB.adminCommand(
        {configureFailPoint: 'leaveIndexBuildUnfinishedForShutdown', mode: 'alwaysOn'}));

    try {
        coll.createIndex({i: 1});
        coll.createIndex({x: 1});
        coll.createIndex({y: 1});
        primaryDB.getLastError(2);
        assert.eq(4, coll.getIndexes().length);

        // Make sure all writes are durable on the secondary so that we can restart it knowing that
        // the index build will be found on startup.
        // Waiting for durable is important for both (A) the record that we started the index build
        // so it is rebuild on restart, and (B) the update to minvalid to show that we've already
        // applied the oplog entry so it isn't replayed. If (A) is present without (B), then there
        // are two ways that the index can be rebuilt on startup and this test is only for the one
        // triggered by (A).
        secondDB.adminCommand({fsync: 1});
    } finally {
        assert.commandWorked(secondDB.adminCommand(
            {configureFailPoint: 'leaveIndexBuildUnfinishedForShutdown', mode: 'off'}));
    }
}

MongoRunner.stopMongod(second);

if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    replTest.start(
        second,
        {
            setParameter:
                {"failpoint.hangAfterSettingUpIndexBuildUnlocked": tojson({mode: "alwaysOn"})}
        },
        /*restart=*/true,
        /*wait=*/true);
} else {
    replTest.start(second,
                   {},
                   /*restart=*/true,
                   /*wait=*/true);
}

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

    if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
        // Verify that we do not wait for the index build to complete on startup.
        assert.eq(size, secondDB.getCollection(collectionName).find({}).itcount());

        // Verify that only the _id index is ready.
        checkLog.containsJson(second, 4585201);
        IndexBuildTest.assertIndexes(secondDB.getCollection(collectionName),
                                     4,
                                     ["_id_"],
                                     ["i_1", "x_1", "y_1"],
                                     {includeBuildUUIDS: true});
    }
} finally {
    assert.commandWorked(second.adminCommand(
        {configureFailPoint: 'hangAfterSettingUpIndexBuildUnlocked', mode: 'off'}));

    // Let index build complete on primary, which replicates a commitIndexBuild to the secondary.
    IndexBuildTest.resumeIndexBuilds(primaryDB);
}

assert.soon(function() {
    return 4 == secondDB.getCollection(collectionName).getIndexes().length;
}, "Index build not resumed after restart", 30000, 50);
replTest.stopSet();
}());
