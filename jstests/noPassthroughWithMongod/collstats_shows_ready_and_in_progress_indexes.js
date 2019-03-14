/**
 * Ensures that the 'collStats' command lists indexes that are ready and in-progress.
 */
(function() {
    'use strict';

    load('jstests/noPassthrough/libs/index_build.js');

    const failpoint = 'hangAfterStartingIndexBuildUnlocked';
    assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

    const conn = db.getMongo();

    const collName = 'collStats';
    const coll = db.getCollection(collName);

    const bulk = coll.initializeUnorderedBulkOp();
    const numDocs = 5;
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({a: i, b: i * i});
    }
    assert.commandWorked(bulk.execute());

    // Start two index builds in the background.
    const awaitParallelShell = startParallelShell(() => {
        db.runCommand({
            createIndexes: 'collStats',
            indexes: [
                {key: {a: 1}, name: 'a_1', background: true},
                {key: {b: 1}, name: 'b_1', background: true}
            ]
        });
    }, conn.port);

    // Wait until both index builds begin.
    IndexBuildTest.waitForIndexBuildToStart(db);

    const collStats = assert.commandWorked(db.runCommand({collStats: collName}));

    // Ensure the existence of the indexes in the following fields: 'indexSizes', 'nindexes' and
    // 'indexDetails'.
    assert.gte(collStats.indexSizes._id_, 0);
    assert.gte(collStats.indexSizes.a_1, 0);
    assert.gte(collStats.indexSizes.b_1, 0);

    assert.eq(3, collStats.nindexes);

    assert.eq(2, collStats.indexBuilds.length);
    assert.eq('a_1', collStats.indexBuilds[0]);
    assert.eq('b_1', collStats.indexBuilds[1]);

    assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));
    awaitParallelShell();
})();
