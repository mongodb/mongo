/**
 * Confirms that background index builds started through applyOps on a primary can be aborted using
 * killop. This is because primaries run background index builds in the foreground when run through
 * applyOps.
 *
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

    IndexBuildTest.pauseIndexBuilds(primary);

    const applyOpsCmd = {
        applyOps: [
            {
              op: 'c',
              ns: testDB.getCollection('$cmd').getFullName(),
              o: {
                  createIndexes: coll.getName(),
                  v: 2,
                  name: 'a_1',
                  key: {a: 1},
                  background: true,
              },
            },
        ]
    };
    const createIdx = startParallelShell(
        'assert.commandWorked(db.adminCommand(' + tojson(applyOpsCmd) + '))', primary.port);

    // When the index build starts, find its op id.
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB);

    // CurOp output should contain the current state of the index builder such as the build UUID
    // and build phase.
    IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, false);

    // Kill the index build. This should have no effect.
    assert.commandWorked(testDB.killOp(opId));

    // Wait for the index build to stop.
    try {
        IndexBuildTest.waitForIndexBuildToStop(testDB);
    } finally {
        IndexBuildTest.resumeIndexBuilds(primary);
    }

    const exitCode = createIdx({checkExitSuccess: false});
    assert.neq(
        0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

    // Check that index was created on the primary despite the attempted killOp().
    IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

    rst.stopSet();
})();
