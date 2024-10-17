/**
 * Tests if there is an index build conflict after a node steps down and the existing index build
 * fails, that the index build from the new primary is not missing index build on the old
 * primary/secondary. The index build conflicts when the new primary independently creates an index
 * with the same name as the old primary.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

// The index build conflict is only an issue when oplogApplicationEnforcesSteadyStateConstraints is
// false. This is false by default outside of our testing.
const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {setParameter: {oplogApplicationEnforcesSteadyStateConstraints: false}}
});
rst.startSet();
rst.initiate();

const dbName = 'test';
const collName = 'coll';
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

const hangFpOnSetup = configureFailPoint(primary, 'hangIndexBuildOnSetupBeforeTakingLocks');
const hangFpOnConflict = configureFailPoint(primary, 'hangAfterIndexBuildConflict');

jsTestLog("Starting index build");
let awaitIndexBuild = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.InterruptedDueToReplStateChange]);

jsTestLog("Waiting for primary to register the index build");
hangFpOnSetup.wait();

jsTestLog("Stepping up the secondary");
assert.commandWorked(secondary.adminCommand({replSetStepUp: 1}));
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);

jsTestLog("Waiting for new primary to start index build with the same name");
let awaitSecondaryIndexBuild =
    IndexBuildTest.startIndexBuild(secondary, secondaryColl.getFullName(), {a: 1}, null, null, 2);
IndexBuildTest.waitForIndexBuildToStart(primaryDB, collName, "a_1");

// Wait for the index builds to conflict on the old primary.
hangFpOnConflict.wait();

// Allow first index build to be cleaned up and index build should no longer have conflict.
hangFpOnSetup.off();
awaitIndexBuild();
hangFpOnConflict.off();
awaitSecondaryIndexBuild();

rst.stopSet();
