// Tests that resuming a change stream with 'fullDocumentBeforeChange: "required"' or
// 'fullDocument: "required"' does not apply the "required" check to the resume-token event. That
// event is consumed by the stream and never returned to the client, so it must not fail the stream
// even if its pre-image is not available.
//
// Resuming with 'startAtOperationTime' is different: the start point is a high-water-mark token, so
// the event at that timestamp is returned to the client and is subject to the "required" check.
//
// @tags: [
//   uses_change_streams,
// ]
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    ChangeStreamWatchMode,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("resuming a change stream with a required pre-/post-image", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = jsTestName();

    let coll;
    let tokenOfOldUpdate;
    let clusterTimeOfOldUpdate;
    let clusterTimeOfNewUpdate;

    function setPreAndPostImages(enabled) {
        assert.commandWorked(
            testDB.runCommand({
                collMod: collName,
                changeStreamPreAndPostImages: {enabled},
            }),
        );
    }

    // Runs 'writeFn' and returns the resume token and cluster time of the update event it produces.
    function captureNextUpdate(cst, writeFn) {
        const cursor = cst.getChangeStream({
            watchMode: ChangeStreamWatchMode.kCollection,
            coll: collName,
        });
        writeFn();
        const event = cst.getOneChange(cursor);
        assert.eq(event.operationType, "update", event);
        return {token: event._id, clusterTime: event.clusterTime};
    }

    // Opens a collection change stream with the given resume and stream options.
    function openStream(cst, resumeOptions, streamOptions) {
        return cst.startWatchingChanges({
            pipeline: [{$changeStream: {...resumeOptions, ...streamOptions}}],
            collection: collName,
            // Use a batch size of 0 so that the event is not returned in the first batch, which
            // cst.getOneChange() would ignore.
            aggregateOptions: {cursor: {batchSize: 0}},
        });
    }

    // The resume points that cause the new update (which has both images) to be returned.
    // 'startAtOperationTime' cannot reference the old update's event directly: it is a
    // high-water-mark point in time, so the event at that timestamp is returned to the client.
    function resumePointsForNewUpdate() {
        return [
            {startAfter: tokenOfOldUpdate},
            {resumeAfter: tokenOfOldUpdate},
            {startAtOperationTime: clusterTimeOfNewUpdate},
        ];
    }

    before(function () {
        coll = assertDropAndRecreateCollection(testDB, collName);

        // 1. Pre-images are off. Make one update and keep its resume token.
        assert.commandWorked(coll.insert({_id: 1, v: 0}));
        withChangeStreamTest(testDB, (cst) => {
            ({token: tokenOfOldUpdate, clusterTime: clusterTimeOfOldUpdate} = captureNextUpdate(
                cst,
                () => {
                    assert.commandWorked(coll.updateOne({_id: 1}, {$set: {v: 1}}));
                },
            ));
        });

        // 2. Enable pre-/post-images. Every later change has both images.
        setPreAndPostImages(true);
        withChangeStreamTest(testDB, (cst) => {
            ({clusterTime: clusterTimeOfNewUpdate} = captureNextUpdate(cst, () => {
                assert.commandWorked(coll.updateOne({_id: 1}, {$set: {v: 2}}));
            }));
        });
    });

    after(function () {
        assertDropCollection(testDB, collName);
    });

    it("does not apply the required check to the resume-token event for fullDocumentBeforeChange", function () {
        // Resuming after the old update must succeed: the only event returned is the new update,
        // which has both images. The old (token) event has no pre-image and must not fail the
        // stream.
        withChangeStreamTest(testDB, (cst) => {
            for (const resumeOptions of resumePointsForNewUpdate()) {
                const cursor = openStream(cst, resumeOptions, {
                    fullDocumentBeforeChange: "required",
                });
                const event = cst.getOneChange(cursor);
                assert.eq(event.operationType, "update", event);
                assert.docEq(event.documentKey, {_id: 1}, event);
                assert.docEq(event.fullDocumentBeforeChange, {_id: 1, v: 1}, event);
            }
        });
    });

    it("does not apply the required check to the resume-token event for fullDocument", function () {
        withChangeStreamTest(testDB, (cst) => {
            for (const resumeOptions of resumePointsForNewUpdate()) {
                // fullDocument: "required" (post-image), which is derived from the pre-image.
                const cursor = openStream(cst, resumeOptions, {fullDocument: "required"});
                const event = cst.getOneChange(cursor);
                assert.eq(event.operationType, "update", event);
                assert.docEq(event.fullDocument, {_id: 1, v: 2}, event);
                assert(event.hasOwnProperty("fullDocument"), event);
            }
        });
    });

    it("applies the required check when resuming with startAtOperationTime", function () {
        // A 'startAtOperationTime' resume point is a high-water-mark token: the event at that
        // timestamp is returned to the client and so is subject to the "required" check. The old
        // update has no pre-image, so the stream must fail.
        withChangeStreamTest(testDB, (cst) => {
            try {
                const cursor = openStream(
                    cst,
                    {startAtOperationTime: clusterTimeOfOldUpdate},
                    {fullDocumentBeforeChange: "required"},
                );
                cst.getOneChange(cursor);
            } catch (e) {
                assert.eq(e.code, ErrorCodes.NoMatchingDocument, e);
            }
        });
    });

    it("still applies the required check to a later event with no pre-image", function () {
        // Make an update whose pre-image is available, then disable pre-images and make an update
        // whose pre-image is not. Resuming from a token before the second update must fail: that
        // event is returned to the client and so is subject to the "required" check.
        let tokenBeforeDisabledUpdate;
        withChangeStreamTest(testDB, (cst) => {
            tokenBeforeDisabledUpdate = captureNextUpdate(cst, () => {
                assert.commandWorked(coll.updateOne({_id: 1}, {$set: {v: 3}}));
            }).token;
        });
        setPreAndPostImages(false);
        assert.commandWorked(coll.updateOne({_id: 1}, {$set: {v: 4}}));

        // The failure may surface either while opening the stream (if the first batch already
        // contains the offending event) or on a subsequent getMore, so wrap both in the check.
        withChangeStreamTest(testDB, (cst) => {
            try {
                const cursor = openStream(
                    cst,
                    {startAfter: tokenBeforeDisabledUpdate},
                    {fullDocumentBeforeChange: "required"},
                );
                cst.getOneChange(cursor);
            } catch (e) {
                assert.eq(e.code, ErrorCodes.NoMatchingDocument, e);
            }
        });
    });
});
