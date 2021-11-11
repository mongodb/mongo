/**
 * Tests that an initial syncing node can drop ready indexes while having in-progress index builds
 * during the oplog replay phase.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = jsTestName();

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        }
    ]
});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
rst.awaitReplication();

// Forcefully re-sync the secondary.
let secondary = rst.restart(1, {
    startClean: true,
    setParameter: {
        'failpoint.initialSyncHangDuringCollectionClone': tojson(
            {mode: 'alwaysOn', data: {namespace: "admin.system.version", numDocsToClone: 0}}),
        'failpoint.initialSyncHangAfterDataCloning': tojson({mode: 'alwaysOn'}),
    }
});

// Wait until we block on cloning 'admin.system.version'.
checkLog.containsJson(secondary, 21138);

IndexBuildTest.pauseIndexBuilds(secondary);

// Start an index build on the primary, so that when initial sync is cloning the user collection it
// sees an in-progress two phase index build.
TestData.dbName = dbName;
TestData.collName = collName;
const awaitIndexBuild = startParallelShell(() => {
    const coll = db.getSiblingDB(TestData.dbName).getCollection(TestData.collName);
    assert.commandWorked(coll.createIndex({b: 1}, {}, "votingMembers"));
}, primary.port);

IndexBuildTest.waitForIndexBuildToStart(db, collName, "b_1");

// Finish the collection cloning phase on the initial syncing node.
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangDuringCollectionClone", mode: "off"}));

// The initial syncing node is ready to enter the oplog replay phase.
checkLog.containsJson(secondary, 21184);

// The initial syncing node has {b: 1} in-progress.
checkLog.containsJson(secondary, 20384, {
    properties: function(properties) {
        return properties.name == "b_1";
    }
});

// The initial syncing node will drop {a: 1} during the oplog replay phase, while having {b: 1}
// in-progress.
assert.commandWorked(coll.dropIndex({a: 1}));

// Let the initial syncing node start the oplog replay phase.
assert.commandWorked(
    secondary.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

// Dropping {a: 1} on the initial syncing node.
checkLog.containsJson(secondary, 20344, {indexes: "\"a_1\""});

rst.awaitReplication();
rst.awaitSecondaryNodes();

IndexBuildTest.resumeIndexBuilds(secondary);

awaitIndexBuild();

IndexBuildTest.assertIndexes(
    coll, /*numIndexes=*/2, /*readyIndexes=*/["_id_", "b_1"], /*notReadyIndexes=*/[]);

const secondaryColl = secondary.getDB(dbName).getCollection(collName);
IndexBuildTest.assertIndexes(
    secondaryColl, /*numIndexes=*/2, /*readyIndexes=*/["_id_", "b_1"], /*notReadyIndexes=*/[]);

rst.stopSet();
}());
