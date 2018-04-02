// Test that change streams involving 4.0 features are not allowed to be opened when the FCV is
// older than 4.0.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");           // For startSetIfSupportsReadMajority.
    load("jstests/libs/change_stream_util.js");  // For ChangeStreamTest.

    const rst = new ReplSetTest({
        nodes: 2,
        nodeOpts: {binVersion: "latest"},
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        rst.stopSet();
        return;
    }

    rst.initiate();

    const testDB = rst.getPrimary().getDB(jsTestName());
    const adminDB = rst.getPrimary().getDB("admin");

    // Explicitly set feature compatibility version 4.0.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Open and test a change stream using 4.0 features.
    const cst = new ChangeStreamTest(testDB);
    const cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    assert.writeOK(testDB["random_coll"].insert({_id: 0}));
    let change = cst.getOneChange(cursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 0);

    // Set the feature compatibility version to 3.6.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // An already created change stream should continue to work.
    assert.writeOK(testDB["other_random_coll"].insert({_id: 0}));
    change = cst.getOneChange(cursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 0);

    // Creating a new change stream with a 4.0 feature should fail.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    rst.stopSet();
}());
