/**
 * Confirms that index creation on a secondary does not use the optimization for empty collections
 * that a primary would apply to two phase index builds.
 *
 * This test starts an index build on a non-empty collection but clears the collection
 * before the index build is added to the catalog. This causes the secondary to see an empty
 * collection.
 *
 * This test should also work when the primary builds the index single-phased. The secondary should
 * be able to optimize for the empty collection case and build the index inlined.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

// Use a 3-node replica set config to ensure that the primary waits for the secondaries when the
// commit quorum is in effect.
const rst = new ReplSetTest({nodes: 3});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

const res = assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangBeforeInitializingIndexBuild', mode: 'alwaysOn'}));
const failpointTimesEntered = res.count;

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "hangBeforeInitializingIndexBuild",
        timesEntered: failpointTimesEntered + 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Remove the document from the collection so that the secondary sees an empty collection.
    assert.commandWorked(coll.remove({a: 1}));
} finally {
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: 'hangBeforeInitializingIndexBuild', mode: 'off'}));
}

// Expect successful createIndex command invocation in parallel shell. A new index should be
// present on the primary.
createIdx();
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

rst.awaitReplication();
const secondary = rst.getSecondary();
const secondaryDB = primary.getDB(testDB.getName());
const secondaryColl = secondary.getCollection(coll.getFullName());
IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
