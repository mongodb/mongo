/**
 * Tests that a failing index build on a secondary node causes the primary node to abort the build.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const primaryDB = primary.getDB('test');
const primaryColl = primaryDB.getCollection('test');

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(primaryDB.getName());
const secondaryColl = secondaryDB.getCollection('test');

// Avoid optimization on empty colls.
assert.commandWorked(primaryColl.insert({a: 1}));

// Pause the index builds on the secondary, using the 'hangAfterStartingIndexBuild' failpoint.
const failpointHangAfterInit = configureFailPoint(secondaryDB, "hangAfterInitializingIndexBuild");

// Create the index and start the build. Set commitQuorum of 2 nodes explicitly, otherwise as only
// primary is voter, it would immediately commit.
const createIdx = IndexBuildTest.startIndexBuild(primary,
                                                 primaryColl.getFullName(),
                                                 {a: 1},
                                                 {},
                                                 [ErrorCodes.IndexBuildAborted],
                                                 /*commitQuorum: */ 2);
const kIndexName = 'a_1';

failpointHangAfterInit.wait();

// Extract the index build UUID. Use assertIndexesSoon to retry until the oplog applier is done with
// the entry, and the index is visible to listIndexes. The failpoint does not ensure this.
const buildUUID =
    IndexBuildTest
        .assertIndexesSoon(
            secondaryColl, 2, ['_id_'], [kIndexName], {includeBuildUUIDs: true})[kIndexName]
        .buildUUID;

const failSecondaryBuild =
    configureFailPoint(secondaryDB,
                       "failIndexBuildWithError",
                       {buildUUID: buildUUID, error: ErrorCodes.OutOfDiskSpace});

// Unblock index builds, causing the failIndexBuildWithError failpoint to throw an error.
failpointHangAfterInit.off();

// ErrorCodes.IndexBuildAborted was specified as the expected failure in startIndexBuild.
createIdx();

failSecondaryBuild.off();

// Wait for the builds to be unregistered before asserting indexes.
IndexBuildTest.waitForIndexBuildToStop(primaryDB, primaryColl.getName(), kIndexName);
IndexBuildTest.waitForIndexBuildToStop(secondaryDB, secondaryColl.getName(), kIndexName);

// Assert index does not exist.
IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_'], []);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_'], []);

rst.stopSet();
})();
