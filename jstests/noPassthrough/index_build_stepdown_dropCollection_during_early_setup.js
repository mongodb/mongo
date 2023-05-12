/**
 * Starts an index build, steps down the primary before the index build has completed its setup (and
 * made other replicas aware of the index build), and drop the collection the index is being built
 * on. This exercises a path described in SERVER-77025 whereby applying a DDL operation (like
 * dropCollection) on the secondary conflicts with the ongoing index build. This test confirms that
 * replication waits until the index build is not present anymore, and then retries dropCollection
 * and succeeds.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");            // For "configureFailPoint()"
load("jstests/libs/parallelTester.js");             // For "startParallelShell()"
load("jstests/noPassthrough/libs/index_build.js");  // For "IndexBuildTest"

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB("test");
const primaryColl = primaryDB.getCollection("coll");
assert.commandWorked(primaryDB.setLogLevel(1, "replication"));

assert.commandWorked(primaryColl.insert({_id: 1, a: 1}));
rst.awaitReplication();

// Enable fail point which makes index build hang during setup, simulating a condition where the
// index build is registered, but not yet replicated.
const fp = configureFailPoint(primary, "hangIndexBuildOnSetupBeforeTakingLocks");

const waitForIndexBuildToErrorOut = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, {}, [ErrorCodes.InterruptedDueToReplStateChange]);

fp.wait();

// Step down the node, while the index build is set up in memory but the "startIndexBuild" entry
// hasn't replicated.
assert.commandWorked(primaryDB.adminCommand({"replSetStepDown": 5 * 60, "force": true}));

rst.waitForPrimary();

// Drop the collection on the new primary. The new primary is not aware of the index build, because
// the old primary hadn't been able to replicate the "startIndexBuild" oplog entry.
const waitForDropCollection = startParallelShell(function() {
    db.getCollection("coll").drop();
}, rst.getPrimary().port);

// Confirm that the old primary, now secondary waits until the index build is not in progress any
// longer before retrying the drop.
// "Waiting for index build(s) to complete on the namespace  before retrying the conflicting
// operation"
assert.soon(() => checkLog.checkContainsOnceJson(rst.getSecondary(), 7702500));

// Resume the index build so it can fail due to InterruptedDueToReplStateChange.
fp.off();

// Confirm that the old primary, now secondary can retry the dropCollection.
// "Acceptable error during oplog application: background operation in progress for namespace"
assert.soon(() => checkLog.checkContainsOnceJson(rst.getSecondary(), 51775));

// dropCollection now succeeds, and the command completes on the primary.
waitForDropCollection();

rst.awaitReplication();

// The index build fails with InterruptedDueToReplStateChange.
waitForIndexBuildToErrorOut();

// Collection doesn't exist.
assert(!rst.getPrimary().getDB("test").getCollectionNames().includes("coll"));
assert(!rst.getSecondary().getDB("test").getCollectionNames().includes("coll"));

rst.stopSet();
})();
