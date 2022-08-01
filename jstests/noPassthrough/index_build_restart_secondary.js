/**
 * Tests restarting a secondary once an index build starts. The index build should complete when the
 * node starts back up.
 *
 * @tags: [
 *   requires_journaling,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const replTest = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on the secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on the secondary. See SERVER-44821.
        },
    ]
});

replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const primaryDB = primary.getDB('test');
const secondaryDB = secondary.getDB('test');

const collectionName = jsTestName();
const coll = primaryDB.getCollection(collectionName);

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; ++i) {
    bulk.insert({i: i, x: i, y: i});
}
assert.commandWorked(bulk.execute({j: true}));

// Make sure the documents make it to the secondary.
replTest.awaitReplication();

if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    // Pause the index build on the primary after replicating the startIndexBuild oplog entry.
    IndexBuildTest.pauseIndexBuilds(primaryDB);
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {y: 1});

    // Wait for build to start on the secondary.
    jsTestLog("waiting for all index builds to start on secondary");
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "i_1");
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "x_1");
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "y_1");
} else {
    assert.commandWorked(secondaryDB.adminCommand(
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
        secondaryDB.adminCommand({fsync: 1});
    } finally {
        assert.commandWorked(secondaryDB.adminCommand(
            {configureFailPoint: 'leaveIndexBuildUnfinishedForShutdown', mode: 'off'}));
    }
}

MongoRunner.stopMongod(secondary);

if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    replTest.start(
        secondary,
        {
            setParameter:
                {"failpoint.hangAfterSettingUpIndexBuildUnlocked": tojson({mode: "alwaysOn"})}
        },
        /*restart=*/true,
        /*wait=*/true);
} else {
    replTest.start(secondary,
                   {},
                   /*restart=*/true,
                   /*wait=*/true);
}

// Make sure secondary comes back.
try {
    assert.soon(function() {
        try {
            secondaryDB.isMaster();  // trigger a reconnect if needed
            return true;
        } catch (e) {
            return false;
        }
    }, "secondary didn't restart", 30000, 1000);

    if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
        // Verify that we do not wait for the index build to complete on startup.
        assert.eq(100, secondaryDB.getCollection(collectionName).find({}).itcount());

        // Verify that only the _id index is ready.
        checkLog.containsJson(secondary, 4585201);
        IndexBuildTest.assertIndexes(secondaryDB.getCollection(collectionName),
                                     4,
                                     ["_id_"],
                                     ["i_1", "x_1", "y_1"],
                                     {includeBuildUUIDS: true});
    }
} finally {
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: 'hangAfterSettingUpIndexBuildUnlocked', mode: 'off'}));

    // Let index build complete on primary, which replicates a commitIndexBuild to the secondary.
    IndexBuildTest.resumeIndexBuilds(primaryDB);
}

assert.soonNoExcept(function() {
    return 4 == secondaryDB.getCollection(collectionName).getIndexes().length;
}, "Index build not resumed after restart", 30000, 50);
replTest.stopSet();
}());
