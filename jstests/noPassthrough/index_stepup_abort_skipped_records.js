/**
 * When a node becomes primary, it verifies in progress index builds and aborts any which have
 * skipped records that still cause key generation errors.
 *
 * @tags: [
 *   featureFlagIndexBuildGracefulErrorHandling,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const primaryDB = primary.getDB('test');
const primaryColl = primaryDB.getCollection('test');

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(primaryDB.getName());
const secondaryColl = secondaryDB.getCollection('test');

// Avoid optimization on empty colls. Invalid 2dsphere key.
assert.commandWorked(primaryColl.insert(
    {geometry: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [-2, -1], [0, 0]]]}}));

// Pause the index builds on the primary, using the 'hangAfterStartingIndexBuild' failpoint.
const failpointHangAfterInit = configureFailPoint(primaryDB, "hangAfterInitializingIndexBuild");

// Create the index and start the build, the secondary will be stepping up, so the command should
// fail due to replication state change.
const createIdx = IndexBuildTest.startIndexBuild(primary,
                                                 primaryColl.getFullName(),
                                                 {geometry: "2dsphere"},
                                                 {},
                                                 [ErrorCodes.InterruptedDueToReplStateChange],
                                                 /*commitQuorum: */ 2);

const kIndexName = "geometry_2dsphere";
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, secondaryColl.getName(), kIndexName);

IndexBuildTest.assertIndexesSoon(primaryColl, 2, ['_id_'], [kIndexName]);
IndexBuildTest.assertIndexesSoon(secondaryColl, 2, ['_id_'], [kIndexName]);

rst.stepUp(secondary);

createIdx();

// The new primary should eventually abort the build.
IndexBuildTest.waitForIndexBuildToStop(primaryDB, primaryColl.getName(), kIndexName);
IndexBuildTest.waitForIndexBuildToStop(secondaryDB, secondaryColl.getName(), kIndexName);

IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

// Verify failure reason is due to step-up check.
checkLog.checkContainsOnceJsonStringMatch(
    secondaryColl, 4656003, "error", "Skipped records retry failed on step-up");

rst.stopSet();
})();
