/**
 * Tests that the commit quorum can be changed during a two-phase index build.
 *
 * @tags: [requires_replication]
 */
(function() {
    load("jstests/noPassthrough/libs/index_build.js");
    load("jstests/libs/check_log.js");

    const replSet = new ReplSetTest({
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

    // Allow the createIndexes command to use the index builds coordinator in single-phase mode.
    replSet.startSet({setParameter: {enableIndexBuildsCoordinatorForCreateIndexesCommand: true}});
    replSet.initiate();

    const testDB = replSet.getPrimary().getDB('test');
    const coll = testDB.twoPhaseIndexBuild;

    const bulk = coll.initializeUnorderedBulkOp();
    const numDocs = 1000;
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({a: i, b: i});
    }
    assert.commandWorked(bulk.execute());

    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: "hangIndexBuildBeforeBuilding", mode: "alwaysOn"}));

    // Starts parallel shell to run the command that will hang.
    const awaitShell = startParallelShell(function() {
        // Use the index builds coordinator for a two-phase index build.
        assert.commandWorked(db.runCommand({
            twoPhaseCreateIndexes: 'twoPhaseIndexBuild',
            indexes: [{key: {a: 1}, name: 'a_1'}],
            commitQuorum: "majority"
        }));
    }, testDB.getMongo().port);

    checkLog.contains(replSet.getPrimary(), "Waiting for index build to complete");

    // Test setting various commit quorums on the index build in our two node replica set.
    assert.commandFailed(testDB.runCommand(
        {setIndexCommitQuorum: 'twoPhaseIndexBuild', indexNames: ['a_1'], commitQuorum: 3}));
    assert.commandFailed(testDB.runCommand({
        setIndexCommitQuorum: 'twoPhaseIndexBuild',
        indexNames: ['a_1'],
        commitQuorum: "someTag"
    }));

    assert.commandWorked(testDB.runCommand(
        {setIndexCommitQuorum: 'twoPhaseIndexBuild', indexNames: ['a_1'], commitQuorum: 0}));
    assert.commandWorked(testDB.runCommand(
        {setIndexCommitQuorum: 'twoPhaseIndexBuild', indexNames: ['a_1'], commitQuorum: 2}));
    assert.commandWorked(testDB.runCommand({
        setIndexCommitQuorum: 'twoPhaseIndexBuild',
        indexNames: ['a_1'],
        commitQuorum: "majority"
    }));

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: "hangIndexBuildBeforeBuilding", mode: "off"}));

    // Wait for the parallel shell to complete.
    awaitShell();

    IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

    replSet.stopSet();
})();
