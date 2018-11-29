/**
 * Confirms that background index builds on a secondary cannot be aborted using killop.
 * @tags: [requires_replication]
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
            },
        ]
    });
    const nodes = rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB('test');
    const coll = testDB.getCollection('test');

    assert.writeOK(coll.insert({a: 1}));

    const secondary = rst.getSecondary();
    IndexBuildTest.pauseIndexBuilds(secondary);

    const createIdx =
        IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {background: true});

    // When the index build starts, find its op id.
    const secondaryDB = secondary.getDB(testDB.getName());
    const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

    // Kill the index build.
    assert.commandWorked(secondaryDB.killOp(opId));

    // Wait for the index build to stop.
    try {
        IndexBuildTest.waitForIndexBuildToStop(secondaryDB);
    } finally {
        IndexBuildTest.resumeIndexBuilds(secondary);
    }

    // Expect successful createIndex command invocation in parallel shell. A new index should be
    // present on the primary.
    createIdx();
    IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

    // Check that no new index has been created on the secondary.
    // This verifies that the index build was aborted rather than successfully completed.
    const secondaryColl = secondaryDB.getCollection(coll.getName());
    IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

    // Index 'a_1' was aborted on the secondary, resulting in a different set of indexes on the
    // secondary compared to the primary. Therefore, we skip the dbhash checking while tearing down
    // the replica set test fixture.
    TestData.skipCheckDBHashes = true;
    rst.stopSet();
})();
