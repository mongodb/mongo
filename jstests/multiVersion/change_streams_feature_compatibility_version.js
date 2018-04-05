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

    // Test both whole-db change streams and cluster-wide change streams.
    const testCases = [{db: testDB, spec: {}}, {db: adminDB, spec: {allChangesForCluster: true}}];

    for (let testCase of testCases) {
        const coll = testDB[jsTestName()];
        coll.drop();

        // Explicitly set feature compatibility version 4.0.
        let res = adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"});
        assert.commandWorked(res);
        const startAtTime = res.$clusterTime.clusterTime;

        // Open and test a change stream using 4.0 features.
        const cst = new ChangeStreamTest(testCase.db);

        const multiCollectionCursor =
            cst.startWatchingChanges({pipeline: [{$changeStream: testCase.spec}], collection: 1});
        const startAtSpec =
            Object.assign({}, testCase.spec, {startAtClusterTime: {ts: startAtTime}});
        const startAtClusterTimeCursor =
            cst.startWatchingChanges({pipeline: [{$changeStream: startAtSpec}], collection: 1});

        assert.writeOK(coll.insert({_id: 0}));
        let change = cst.getOneChange(multiCollectionCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.documentKey._id, 0);

        change = cst.getOneChange(startAtClusterTimeCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.documentKey._id, 0);

        // Set the feature compatibility version to 3.6.
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

        // An already created change stream should continue to work.
        assert.writeOK(coll.insert({_id: 1}));
        change = cst.getOneChange(multiCollectionCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.documentKey._id, 1);

        change = cst.getOneChange(startAtClusterTimeCursor);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(change.documentKey._id, 1);

        // Creating a new change stream with a 4.0 feature should fail.
        assert.commandFailedWithCode(
            testCase.db.runCommand(
                {aggregate: 1, pipeline: [{$changeStream: testCase.spec}], cursor: {}}),
            ErrorCodes.QueryFeatureNotAllowed);

        assert.commandFailedWithCode(testDB.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$changeStream: {startAtClusterTime: {ts: startAtTime}}}],
            cursor: {}
        }),
                                     ErrorCodes.QueryFeatureNotAllowed);
    }

    rst.stopSet();
}());
