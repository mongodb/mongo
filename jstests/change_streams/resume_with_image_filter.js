// Tests that resuming a change stream with an image-dependent $match keeps the resume-token event.
// The image stages enrich the resume event even though it is never returned to the client, so the
// $match (which the optimizer pushes ahead of token verification) sees the image, keeps the event,
// and the stream can verify the resume point instead of failing with ChangeStreamFatalError.
//
// @tags: [
//   uses_change_streams,
// ]
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {describe, it, beforeEach, afterEach} from "jstests/libs/mochalite.js";

describe("resuming a change stream with an image-dependent $match", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = jsTestName();

    let coll;

    function assertFilteredResumption({match, options}) {
        assert.commandWorked(coll.insert({_id: 1, v: 0}));
        assert.commandWorked(
            testDB.runCommand({
                collMod: collName,
                changeStreamPreAndPostImages: {enabled: true},
            }),
        );

        // Capture a resume token whose event has an image.
        let token;
        const captureStream = coll.watch();
        try {
            assert.commandWorked(coll.updateOne({_id: 1}, {$set: {v: 1}}));
            assert.soon(() => captureStream.hasNext());
            const event = captureStream.next();
            assert.eq(event.operationType, "update", event);
            token = event._id;
        } finally {
            captureStream.close();
        }

        // Resume from that token with an image-dependent $match. The resume event must still be
        // enriched, otherwise the $match drops it and the stream fails with
        // ChangeStreamFatalError once it advances past the resume point.
        const stream = coll.watch([{$match: match}], {startAfter: token, ...options});
        try {
            assert.commandWorked(coll.updateOne({_id: 1}, {$set: {v: 2}}));
            assert.soon(() => stream.hasNext());
            const event = stream.next();
            assert.eq(event.operationType, "update", event);
            assert.docEq(event.documentKey, {_id: 1}, event);
        } finally {
            stream.close();
        }
    }

    beforeEach(function () {
        coll = assertDropAndRecreateCollection(testDB, collName);
    });

    afterEach(function () {
        assertDropCollection(testDB, collName);
    });

    it("keeps the resume event for a fullDocumentBeforeChange filter", function () {
        assertFilteredResumption({
            match: {"fullDocumentBeforeChange._id": 1},
            options: {fullDocumentBeforeChange: "required"},
        });
    });

    it("keeps the resume event for a fullDocument filter (required)", function () {
        assertFilteredResumption({
            match: {"fullDocument._id": 1},
            options: {fullDocument: "required"},
        });
    });

    it("keeps the resume event for a fullDocument filter (updateLookup)", function () {
        assertFilteredResumption({
            match: {"fullDocument._id": 1},
            options: {fullDocument: "updateLookup"},
        });
    });
});
