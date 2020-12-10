/**
 * Confirms that creating index creation on an empty collection does not require the thread pool
 * that we use for hybrid index builds. The fail points that we typically use to suspend hybrid
 * should not affect the progress of index builds on empty collections.
 * @tags: [
 *   requires_replication,
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
            slowms: 30000,  // Don't log slow operations on secondary. See SERVER-44821.
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');

// This test uses a non-empty collection as a control by suspending index builds on the thread pool.
// The optimization for an empty collection should not go through the code path for an index build
// that requires a collection scan and the hybrid index build machinery for managing side writes.
IndexBuildTest.pauseIndexBuilds(primary);
const coll = testDB.getCollection('test');
assert.commandWorked(coll.insert({a: 1}));
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    // When the index build starts, find its op id.
    const secondaryDB = primary.getDB(testDB.getName());
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB);

    // Creating an index on an empty collection should not be affected by the fail point we used
    // to suspend index builds.
    const emptyColl = testDB.getCollection('emptyColl');
    assert.commandWorked(testDB.createCollection(emptyColl.getName()));

    // Build index with a writeConcern that ensures the index is finished on all the nodes.
    assert.commandWorked(testDB.runCommand({
        createIndexes: emptyColl.getName(),
        indexes: [{key: {b: 1}, name: 'b_1'}],
        writeConcern: {
            w: nodes.length,
            wtimeout: ReplSetTest.kDefaultTimeoutMS,
        },
    }));
    IndexBuildTest.assertIndexes(emptyColl, 2, ['_id_', 'b_1']);

    const secondary = rst.getSecondary();
    const secondaryEmptyColl = secondary.getCollection(emptyColl.getFullName());
    IndexBuildTest.assertIndexes(secondaryEmptyColl, 2, ['_id_', 'b_1']);

    // Index build optimatization for empty collection is replicated via old-style createIndexes
    // oplog entry.
    const cmdNs = testDB.getCollection('$cmd').getFullName();
    const ops = rst.dumpOplog(
        primary, {op: 'c', ns: cmdNs, 'o.createIndexes': emptyColl.getName(), 'o.name': 'b_1'});
    assert.eq(1,
              ops.length,
              'createIndexes oplog entry not generated for empty collection: ' + tojson(ops));
} finally {
    // Wait for the index build to stop.
    IndexBuildTest.resumeIndexBuilds(primary);
    IndexBuildTest.waitForIndexBuildToStop(testDB);
}

// Expect successful createIndex command invocation in parallel shell. A new index should be
// present on the primary.
createIdx();
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
