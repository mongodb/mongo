/**
 * Initial syncing a node with two phase index builds should immediately build all ready indexes
 * from the sync source and only setup the index builder threads for any unfinished index builds
 * grouped by their buildUUID.
 *
 * Previously, an initial syncing node would start and finish the index build when it applied the
 * "commitIndexBuild" oplog entry, but the primary will no longer send that oplog entry until the
 * commit quorum is satisfied, which may depend on the initial syncing nodes vote.
 *
 * Take into consideration the following scenario where the primary could not achieve the commit
 * quorum without the initial syncing nodes vote:
 * 1. Node A (primary) starts a two-phase index build "x_1" with commit quorum "votingMembers".
 * 2. Node B (secondary) shuts down while building the "x_1" index, preventing the node from sending
 *    its vote to the primary.
 * 3. Node A cannot achieve the commit quorum and is stuck. The "commitIndexBuild" oplog entry does
 *    not get sent to any other nodes.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = jsTest.name();
const collName = "commitQuorumWithInitialSync";

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
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

if (!(IndexBuildTest.supportsTwoPhaseIndexBuild(primary) &&
      IndexBuildTest.indexBuildCommitQuorumEnabled(primary))) {
    jsTestLog(
        'Skipping test because two phase index build and index build commit quorum are not supported.');
    rst.stopSet();
    return;
}

assert.commandWorked(coll.insert({a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
rst.awaitReplication();

// Start multiple index builds using a commit quorum of "votingMembers", but pause the index build
// on the secondary, preventing it from voting to commit the index build.
jsTest.log("Pausing index builds on the secondary");
let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

TestData.dbName = dbName;
TestData.collName = collName;
const awaitFirstIndexBuild = startParallelShell(() => {
    const coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
    assert.commandWorked(coll.createIndex({b: 1}, {}, "votingMembers"));
}, primary.port);

const awaitSecondIndexBuild = startParallelShell(() => {
    const coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
    assert.commandWorked(coll.createIndexes([{c: 1}, {d: 1}], {}, "votingMembers"));
}, primary.port);

const awaitThirdIndexBuild = startParallelShell(() => {
    const coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
    assert.commandWorked(coll.createIndexes([{e: 1}, {f: 1}, {g: 1}], {}, "votingMembers"));
}, primary.port);

// Wait for all the indexes to start building on the primary.
IndexBuildTest.waitForIndexBuildToStart(db, collName, "b_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "c_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "d_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "e_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "f_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "g_1");

// Restart the secondary with a clean data directory to start the initial sync process.
secondary = rst.restart(1, {
    startClean: true,
    setParameter: 'failpoint.initialSyncHangAfterDataCloning=' + tojson({mode: 'alwaysOn'}),
});

// The secondary node will start any in-progress two-phase index builds from the primary before
// starting the oplog replay phase. This ensures that the secondary will send its vote to the
// primary when it is ready to commit the index build. The index build on the secondary will get
// committed once the primary sends the "commitIndexBuild" oplog entry after the commit quorum is
// satisfied with the secondaries vote.
checkLog.containsJson(secondary, 21184);

// Cannot use IndexBuildTest helper functions on the secondary during initial sync.
function checkForIndexes(indexes) {
    for (let i = 0; i < indexes.length; i++) {
        checkLog.containsJson(secondary, 20384, {
            "properties": function(obj) {
                return obj.name === indexes[i];
            }
        });
    }
}
checkForIndexes(["b_1", "c_1", "d_1", "e_1", "f_1", "g_1"]);

// Checks that the index specs have the proper grouping by ensuring that we only start 3 index
// builder threads.
checkLog.containsWithCount(secondary, "Index build initialized", 3);

assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

rst.awaitReplication();
rst.awaitSecondaryNodes();

awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitThirdIndexBuild();

let indexes = secondary.getDB(dbName).getCollection(collName).getIndexes();
assert.eq(8, indexes.length);

indexes = coll.getIndexes();
assert.eq(8, indexes.length);
rst.stopSet();
}());
