// Tests that a resume token from FCV 4.0 cannot be used with the new 'startAfter' option, because
// the old version of the resume token doesn't contain enough information to distinguish an
// invalidate event from the event which generated the invalidate.
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");  // For startSetIfSupportsReadMajority.

    const rst = new ReplSetTest({nodes: 1});
    if (!startSetIfSupportsReadMajority(rst)) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        rst.stopSet();
        return;
    }
    rst.initiate();

    let testDB = rst.getPrimary().getDB(jsTestName());
    let coll = testDB.change_stream_upgrade;

    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

    (function testInvalidatesOldFCV() {
        // Open a change stream in 4.0 compatibility version. This stream should be using the old
        // version of resume tokens which cannot distinguish between a drop and the invalidate that
        // follows the drop.
        assert.commandWorked(testDB.runCommand({create: coll.getName()}));
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

        // Only on FCV 4.0 - these two resume tokens should be the same. Because they cannot be
        // distinguished, any attempt to resume or start a new stream should immediately return
        // invalidate.
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
    }());

    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

    (function testInvalidatesNewFCV() {
        // Open a change stream in 4.2 compatibility version. This stream should be using the new
        // version of resume tokens which *can* distinguish between a drop and the invalidate that
        // follows the drop.
        assert.commandWorked(testDB.runCommand({create: coll.getName()}));
        const changeStream = coll.watch();
        coll.drop();

        assert.soon(() => changeStream.hasNext());
        let change = changeStream.next();
        assert.eq(change.operationType, "drop", tojson(change));
        const resumeTokenFromDrop = change._id;

        assert.soon(() => changeStream.hasNext());
        change = changeStream.next();
        assert.eq(change.operationType, "invalidate", tojson(change));
        const resumeTokenFromInvalidate = change._id;

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
        change = newStream.next();
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
    }());

    rst.stopSet();
}());
