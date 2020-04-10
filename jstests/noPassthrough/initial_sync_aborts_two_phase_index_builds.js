/**
 * Verifies that an initial syncing node can abort in-progress two phase index builds during the
 * oplog replay phase.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = jsTest.name();
const collName = "test";

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

if (!(IndexBuildTest.supportsTwoPhaseIndexBuild(primary) &&
      IndexBuildTest.indexBuildCommitQuorumEnabled(primary))) {
    jsTestLog(
        'Skipping test because two phase index build and index build commit quorum are not supported.');
    rst.stopSet();
    return;
}

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
rst.awaitReplication();

// Forcefully re-sync the secondary.
let secondary = rst.restart(1, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangDuringCollectionClone': tojson(
            {mode: 'alwaysOn', data: {namespace: "admin.system.version", numDocsToClone: 0}}),
    }
});

// Wait until we block on cloning 'admin.system.version'.
checkLog.containsJson(secondary, 21138);

assert.commandWorked(coll.insert({a: 2}));
assert.commandWorked(coll.dropIndex({a: 1}));

IndexBuildTest.pauseIndexBuilds(secondary);

// Start an index build on the primary, so that when initial sync is cloning the user collection it
// sees an in-progress two phase index build.
TestData.dbName = dbName;
TestData.collName = collName;
const awaitIndexBuild = startParallelShell(() => {
    const coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
    assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
}, primary.port);

IndexBuildTest.waitForIndexBuildToStart(db, collName, "a_1");

assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

rst.awaitReplication();
rst.awaitSecondaryNodes();

// Check the that secondary hit the background operation in progress error.
checkLog.containsJson(secondary, 23879, {reason: "Aborting index builds during initial sync"});

IndexBuildTest.resumeIndexBuilds(secondary);

awaitIndexBuild();

let indexes = secondary.getDB(dbName).getCollection(collName).getIndexes();
assert.eq(2, indexes.length);

indexes = coll.getIndexes();
assert.eq(2, indexes.length);

rst.stopSet();
return;
}());
