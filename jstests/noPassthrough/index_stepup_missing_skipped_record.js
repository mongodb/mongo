/**
 * When a node steps up to primary, it verifies in progress index builds and aborts any which have
 * skipped records that still cause key generation errors. Verify that we do not invariant when
 * there are skipped records that are longer found in the collection and those index builds are not
 * aborted.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

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

assert.commandWorked(primaryColl.remove(
    {geometry: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [-2, -1], [0, 0]]]}}));

rst.stepUp(secondary);

createIdx();

// Skipped records that are no longer found cannot cause key generation errors, therefore index
// builds are not aborted.
IndexBuildTest.assertIndexesSoon(primaryColl, 2, ['_id_', kIndexName]);
IndexBuildTest.assertIndexesSoon(secondaryColl, 2, ['_id_', kIndexName]);

rst.stopSet();
