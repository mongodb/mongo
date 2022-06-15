/**
 * Tests restarting a secondary once an index build starts. The index build should complete when the
 * node starts back up.
 *
 * @tags: [
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
// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

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

// Pause the index build on the primary after replicating the startIndexBuild oplog entry.
IndexBuildTest.pauseIndexBuilds(primaryDB);
const indexi = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {i: 1});
const indexx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});
const indexy = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {y: 1});

// Wait for build to start on the secondary.
jsTestLog("Waiting for all index builds to start on the secondary");
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "i_1");
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "x_1");
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "y_1");

replTest.stop(secondary);
replTest.start(secondary,
               {
                   setParameter: {
                       "failpoint.hangAfterSettingUpIndexBuildUnlocked": tojson({mode: "alwaysOn"}),
                   }
               },
               true /* restart */);

// Verify that we do not wait for the index build to complete on startup.
assert.soonNoExcept(() => {
    IndexBuildTest.assertIndexes(secondaryDB.getCollection(collectionName),
                                 4,
                                 ["_id_"],
                                 ["i_1", "x_1", "y_1"],
                                 {includeBuildUUIDs: true});
    return true;
});

assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: 'hangAfterSettingUpIndexBuildUnlocked', mode: 'off'}));

// Let index build complete on primary, which replicates a commitIndexBuild to the secondary.
IndexBuildTest.resumeIndexBuilds(primaryDB);

assert.soonNoExcept(function() {
    return 4 === secondaryDB.getCollection(collectionName).getIndexes().length;
}, "Index build did not complete after restart");
indexi();
indexx();
indexy();

replTest.stopSet();
}());
