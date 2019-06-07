// Tests that a resume token from FCV 4.0 cannot be used with the new 'startAfter' option, because
// the old version of the resume token doesn't contain enough information to distinguish an
// invalidate event from the event which generated the invalidate.
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
    load("jstests/multiVersion/libs/change_stream_hwm_helpers.js");  // For ChangeStreamHWMHelpers.
    load("jstests/multiVersion/libs/multi_rs.js");                   // For upgradeSet.
    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    const preBackport40Version = ChangeStreamHWMHelpers.preBackport40Version;
    const latest42Version = ChangeStreamHWMHelpers.latest42Version;

    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: preBackport40Version}});
    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        rst.stopSet();
        return;
    }
    rst.initiate();

    let testDB = rst.getPrimary().getDB(jsTestName());
    let coll = testDB.change_stream_upgrade;

    // Up- or downgrades the replset and then refreshes our references to the test collection.
    function refreshReplSet(version) {
        // Upgrade the set and wait for it to become available again.
        rst.upgradeSet({binVersion: version});
        rst.awaitReplication();

        // Having upgraded the cluster, reacquire references to the db and collection.
        testDB = rst.getPrimary().getDB(jsTestName());
        coll = testDB.change_stream_upgrade;
    }

    // Creates a collection, drops it, and returns the resulting 'drop' and 'invalidate' tokens.
    function generateDropAndInvalidateTokens() {
        assertDropAndRecreateCollection(testDB, coll.getName());
        const streamStartedOnOldFCV = coll.watch();
        coll.drop();

        assert.soon(() => streamStartedOnOldFCV.hasNext());
        let change = streamStartedOnOldFCV.next();
        assert.eq(change.operationType, "drop", tojson(change));
        const resumeTokenFromDrop = change._id;

        assert.soon(() => streamStartedOnOldFCV.hasNext());
        change = streamStartedOnOldFCV.next();
        assert.eq(change.operationType, "invalidate", tojson(change));
        const resumeTokenFromInvalidate = change._id;

        return [resumeTokenFromDrop, resumeTokenFromInvalidate];
    }

    function testInvalidateV0(resumeTokenFromDrop, resumeTokenFromInvalidate) {
        // These two resume tokens should be the same. Because they cannot be distinguished, any
        // attempt to resume or start a new stream should immediately return invalidate.
        assert.eq(resumeTokenFromDrop, resumeTokenFromInvalidate);
        for (let token of[resumeTokenFromDrop, resumeTokenFromInvalidate]) {
            let newStream = coll.watch([], {startAfter: token, collation: {locale: "simple"}});
            assert.soon(() => newStream.hasNext());
            assert.eq(newStream.next().operationType, "invalidate");

            // Test the same thing but with 'resumeAfter' instead of 'startAfter'.
            newStream = coll.watch([], {resumeAfter: token, collation: {locale: "simple"}});
            assert.soon(() => newStream.hasNext());
            assert.eq(newStream.next().operationType, "invalidate");
        }
    }

    function testInvalidateV1(resumeTokenFromDrop, resumeTokenFromInvalidate) {
        // This stream should be using the new version of resume tokens which *can* distinguish a
        // drop from the invalidate that follows it. Recreate the collection with the same name and
        // insert a document.
        assert.commandWorked(testDB.runCommand({create: coll.getName()}));
        assert.commandWorked(coll.insert({_id: "insert after drop"}));

        assert.neq(resumeTokenFromDrop,
                   resumeTokenFromInvalidate,
                   () => tojson(resumeTokenFromDrop) + " should not equal " +
                       tojson(resumeTokenFromInvalidate));
        let newStream =
            coll.watch([], {startAfter: resumeTokenFromDrop, collation: {locale: "simple"}});
        assert.soon(() => newStream.hasNext());
        assert.eq(newStream.next().operationType, "invalidate");

        newStream =
            coll.watch([], {startAfter: resumeTokenFromInvalidate, collation: {locale: "simple"}});
        assert.soon(() => newStream.hasNext());
        const change = newStream.next();
        assert.eq(change.operationType, "insert");
        assert.eq(change.documentKey._id, "insert after drop");

        // Test the same thing but with 'resumeAfter' instead of 'startAfter'. This should see an
        // invalidate on the first, and reject the second.
        newStream =
            coll.watch([], {resumeAfter: resumeTokenFromDrop, collation: {locale: "simple"}});
        assert.soon(() => newStream.hasNext());
        assert.eq(newStream.next().operationType, "invalidate");
        const error = assert.throws(
            () => coll.watch(
                [], {resumeAfter: resumeTokenFromInvalidate, collation: {locale: "simple"}}));
        assert.eq(error.code, ErrorCodes.InvalidResumeToken);
    }

    // We will test 'drop' and 'invalidate' tokens for resume token formats v0 and v1.
    let resumeTokenFromDropV0, resumeTokenFromInvalidateV0;
    let resumeTokenFromDropV1, resumeTokenFromInvalidateV1;

    // We start on 'preBackport40Version'. Generate v0 'drop' and 'invalidate' resume tokens.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    [resumeTokenFromDropV0, resumeTokenFromInvalidateV0] = generateDropAndInvalidateTokens();

    // Now upgrade the set to 'latest42Version' with FCV 4.0.
    refreshReplSet(latest42Version);
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    // Confirm that the v0 tokens behave as expected for binary 4.2 FCV 4.0.
    testInvalidateV0(resumeTokenFromDropV0, resumeTokenFromInvalidateV0);

    // Confirm that new tokens generated by binary 4.2 in FCV 4.0 are v1 rather than v0.
    [resumeTokenFromDropV1, resumeTokenFromInvalidateV1] = generateDropAndInvalidateTokens();
    testInvalidateV1(resumeTokenFromDropV1, resumeTokenFromInvalidateV1);

    // Now upgrade the set to 'latest42Version' with FCV 4.2.
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

    // Confirm that the v0 tokens behave as expected for binary 4.2 FCV 4.2.
    testInvalidateV0(resumeTokenFromDropV0, resumeTokenFromInvalidateV0);

    // Confirm that new tokens generated by binary 4.2 in FCV 4.2 are v1 rather than v0.
    [resumeTokenFromDropV1, resumeTokenFromInvalidateV1] = generateDropAndInvalidateTokens();
    testInvalidateV1(resumeTokenFromDropV1, resumeTokenFromInvalidateV1);

    rst.stopSet();
}());
