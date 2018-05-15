// Test that change streams involving 4.0 features are not allowed to be opened when the FCV is
// older than 4.0.
(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");     // For ChangeStreamTest.
    load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.
    load("jstests/replsets/rslib.js");              // For startSetIfSupportsReadMajority.

    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: "last-stable"},
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        rst.stopSet();
        return;
    }

    rst.initiate();

    function assertResumeTokenUsesStringFormat(resumeToken) {
        assert.neq(resumeToken._data, undefined);
        assert.eq(typeof resumeToken._data, "string", tojson(resumeToken));
    }

    function assertResumeTokenUsesBinDataFormat(resumeToken) {
        assert.neq(resumeToken._data, undefined);
        assert(resumeToken._data instanceof BinData, tojson(resumeToken));
    }

    let testDB = rst.getPrimary().getDB(jsTestName());
    let adminDB = rst.getPrimary().getDB("admin");
    let coll = testDB[jsTestName()];

    let cst = new ChangeStreamTest(testDB);
    let adminCST = new ChangeStreamTest(adminDB);

    // We can't open a change stream on a non-existent database on last-stable, so we insert a dummy
    // document to create the database.
    // TODO BACKPORT-34138 Remove this check once the change has been backported.
    assert.writeOK(testDB.dummy.insert({_id: "dummy"}));

    // Open a change stream against a 3.6 binary. We will use the resume token from this stream to
    // resume the stream once the set has been upgraded.
    let streamOnOldVersion =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: coll.getName()});
    assert.writeOK(coll.insert({_id: 0}));

    let change = cst.getOneChange(streamOnOldVersion);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 0);

    // Extract the resume token and test that it is using the old resume token format using
    // BinData.
    const resumeTokenFromLastStable = change._id;
    assertResumeTokenUsesBinDataFormat(resumeTokenFromLastStable);

    // Test that new query features are banned on 3.6.
    const failedResponse = assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {aggregate: 1, pipeline: [{$changeStream: {allChangesForCluster: true}}], cursor: {}}),
        40415);

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {startAtOperationTime: failedResponse.operationTime}}],
        cursor: {}
    }),
                                 40415);

    // Upgrade the set to the new binary version, but keep the feature compatibility version at
    // 3.6.
    rst.upgradeSet({binVersion: "latest"});
    testDB = rst.getPrimary().getDB(jsTestName());
    adminDB = rst.getPrimary().getDB("admin");
    coll = testDB[jsTestName()];
    cst = new ChangeStreamTest(testDB);
    adminCST = new ChangeStreamTest(adminDB);

    // Test that we can resume the stream on the new binaries.
    streamOnOldVersion = cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromLastStable}}],
        collection: coll.getName()
    });

    assert.writeOK(coll.insert({_id: 1}));

    change = cst.getOneChange(streamOnOldVersion);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 1);

    // Test that the stream is still using the old resume token format using BinData.
    assertResumeTokenUsesBinDataFormat(change._id);

    // Explicitly set feature compatibility version 4.0. Remember the cluster time from that
    // response to use later.
    const startTime =
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "4.0"}))
            .operationTime;

    // Test that we can now use 4.0 features to open a stream.
    const wholeDbCursor =
        cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});
    const wholeClusterCursor = adminCST.startWatchingChanges(
        {pipeline: [{$changeStream: {allChangesForCluster: true}}], collection: 1});
    const cursorStartedWithTime = cst.startWatchingChanges({
        pipeline: [{$changeStream: {startAtOperationTime: startTime}}],
        collection: coll.getName()
    });

    assert.writeOK(coll.insert({_id: 2}));

    // Test that the stream opened in FCV 3.6 continues to work and still generates tokens in the
    // old format.
    change = cst.getOneChange(streamOnOldVersion);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 2);
    assertResumeTokenUsesBinDataFormat(change._id);

    // Test all the newly created streams can see an insert.
    change = cst.getOneChange(wholeDbCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 2);

    change = adminCST.getOneChange(wholeClusterCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 2);

    change = cst.getOneChange(cursorStartedWithTime);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 2);

    // Test that the resume token is using the new format and has type string while FCV is 4.0.
    const resumeToken = change._id;
    assertResumeTokenUsesStringFormat(resumeToken);

    // Test that we can resume with the resume token with either format, either against the entire
    // DB, the entire cluster, or against the single collection.
    assert.doesNotThrow(() => cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromLastStable}}],
        collection: 1
    }));
    assert.doesNotThrow(() => adminCST.startWatchingChanges({
        pipeline:
            [{$changeStream: {allChangesForCluster: true, resumeAfter: resumeTokenFromLastStable}}],
        collection: 1
    }));
    assert.doesNotThrow(() => cst.startWatchingChanges({
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromLastStable}}],
        collection: coll.getName()
    }));
    assert.doesNotThrow(
        () => cst.startWatchingChanges(
            {pipeline: [{$changeStream: {resumeAfter: resumeToken}}], collection: 1}));
    assert.doesNotThrow(() => adminCST.startWatchingChanges({
        pipeline: [{$changeStream: {allChangesForCluster: true, resumeAfter: resumeToken}}],
        collection: 1
    }));
    assert.doesNotThrow(
        () => cst.startWatchingChanges(
            {pipeline: [{$changeStream: {resumeAfter: resumeToken}}], collection: coll.getName()}));

    // Set the feature compatibility version to 3.6.
    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // Test that existing streams continue, but still generate resume tokens in the new format.
    assert.writeOK(coll.insert({_id: 3}));
    change = cst.getOneChange(wholeDbCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 3);
    assertResumeTokenUsesStringFormat(change._id);

    change = adminCST.getOneChange(wholeClusterCursor);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 3);
    assertResumeTokenUsesStringFormat(change._id);

    change = cst.getOneChange(cursorStartedWithTime);
    assert.eq(change.operationType, "insert", tojson(change));
    assert.eq(change.documentKey._id, 3);
    assertResumeTokenUsesStringFormat(change._id);

    // Creating a new change stream with a 4.0 feature should fail.
    assert.commandFailedWithCode(
        testDB.runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);
    assert.commandFailedWithCode(
        adminDB.runCommand(
            {aggregate: 1, pipeline: [{$changeStream: {allChangesForCluster: true}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {startAtOperationTime: startTime}}],
        cursor: {}
    }),
                                 ErrorCodes.QueryFeatureNotAllowed);

    // Test that resuming a change stream opened on FCV 4.0 should continue to work, though a
    // whole-db or whole-cluster cursor cannot be resumed.
    assert.commandFailedWithCode(
        testDB.runCommand(
            {aggregate: 1, pipeline: [{$changeStream: {resumeAfter: resumeToken}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);
    assert.commandFailedWithCode(adminDB.runCommand({
        aggregate: 1,
        pipeline: [{$changeStream: {allChangesForCluster: true, resumeAfter: resumeToken}}],
        cursor: {}
    }),
                                 ErrorCodes.QueryFeatureNotAllowed);
    let resumedOnFCV36With40BinaryResumeToken;
    assert.doesNotThrow(() => {
        resumedOnFCV36With40BinaryResumeToken = coll.watch([], {resumeAfter: resumeToken});
    });
    assert.soon(() => resumedOnFCV36With40BinaryResumeToken.hasNext());
    change = resumedOnFCV36With40BinaryResumeToken.next();
    assertResumeTokenUsesBinDataFormat(change._id);

    // Test that resuming a change stream with the original resume token still works.
    let resumedWith36Token;
    assert.doesNotThrow(() => {
        resumedWith36Token = coll.watch([], {resumeAfter: resumeTokenFromLastStable});
    });
    assert.soon(() => resumedWith36Token.hasNext());
    change = resumedWith36Token.next();
    assertResumeTokenUsesBinDataFormat(change._id);

    rst.stopSet();
}());
