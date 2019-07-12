/**
 * Confirms that background index builds on a primary can be aborted using killop
 * on the client connection operation when the IndexBuildsCoordinator is enabled.
 * @tags: [requires_replication]
 */
(function() {
    "use strict";

    load('jstests/libs/check_log.js');
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

    const enableIndexBuildsCoordinator =
        assert
            .commandWorked(primary.adminCommand(
                {getParameter: 1, enableIndexBuildsCoordinatorForCreateIndexesCommand: 1}))
            .enableIndexBuildsCoordinatorForCreateIndexesCommand;
    if (!enableIndexBuildsCoordinator) {
        jsTestLog(
            'IndexBuildsCoordinator not enabled for index creation on primary, skipping test.');
        rst.stopSet();
        return;
    }

    assert.writeOK(coll.insert({a: 1}));

    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));

    const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

    checkLog.contains(
        primary,
        'index build: starting on ' + coll.getFullName() + ' properties: { v: 2, key: { a:');

    try {
        // When the index build starts, find its op id. This will be the op id of the client
        // connection, not the thread pool task managed by IndexBuildsCoordinatorMongod.
        const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), 'a_1');

        // Kill the index build.
        assert.commandWorked(testDB.killOp(opId));
    } finally {
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));
    }

    // Wait for the index build to stop.
    IndexBuildTest.waitForIndexBuildToStop(testDB);

    const exitCode = createIdx({checkExitSuccess: false});
    assert.neq(
        0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

    checkLog.contains(primary, 'IndexBuildAborted: Index build aborted: ');

    // Check that no new index has been created.  This verifies that the index build was aborted
    // rather than successfully completed.
    IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

    rst.stopSet();
})();
