// Confirm that $changeStream can only run on an entire database if 'enableTestCommands' is true.
// Because the $changeStream stage requires a replica set to run, we tag this test as
// requires_replication.
// @tags: [requires_replication,requires_journaling]

// TODO SERVER-34283: remove this test once whole-database $changeStream is feature-complete.
(function() {
    'use strict';

    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    let testDB = null;
    let rst = null;

    // Creates and initiates a new ReplSetTest with a test database.
    function startNewReplSet(name) {
        rst = new ReplSetTest({name: name, nodes: 1});

        if (!startSetIfSupportsReadMajority(rst)) {
            rst.stopSet();
            return false;
        }
        rst.initiate();

        testDB = rst.getPrimary().getDB(jsTestName());
        assert.commandWorked(testDB.test.insert({_id: 0}));

        return rst;
    }

    jsTest.setOption('enableTestCommands', false);

    if (!startNewReplSet("prod")) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Confirm that we can run $changeStream on an individual collection with
    // 'enableTestCommands:false', but not on the entire db.
    assert.commandWorked(
        testDB.runCommand({aggregate: "test", pipeline: [{$changeStream: {}}], cursor: {}}));
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    rst.stopSet();

    jsTest.setOption('enableTestCommands', true);
    assert(startNewReplSet("test"));

    // Confirm that we can run $changeStream on an individual collection and the entire db with
    // 'enableTestCommands:true'.
    assert.commandWorked(
        testDB.runCommand({aggregate: "test", pipeline: [{$changeStream: {}}], cursor: {}}));
    assert.commandWorked(
        testDB.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}));

    rst.stopSet();
})();