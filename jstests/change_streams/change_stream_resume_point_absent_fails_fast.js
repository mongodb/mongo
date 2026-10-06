/**
 * Tests that a resumed change stream whose filter excludes the resume point fails fast, rather than
 * having to wait for an event to arrive before it can determine that the resume token is absent.
 *
 * The high water mark the stage consults comes from a different source per topology: the oplog
 * cursor on a replica set, and the AsyncResultsMerger on a router. Running this test in both
 * replica-set and sharded-cluster suites covers both sources, and for the router the v1 and v2
 * change stream readers are exercised by the corresponding passthrough suites.
 *
 * Also covers the cases in which the high water mark advancing past the resume point must NOT fail
 * the stream, because the resume point was in fact observed.
 *
 * @tags: [
 *   uses_change_streams,
 *   # The fail-fast behavior relies on reading the resume point from the primary (or on the router
 *   # observing the shards' postBatchResumeTokens), which is not guaranteed with secondary reads.
 *   assumes_read_preference_unchanged,
 *   # The fail-fast behavior was introduced in this branch and is not present in older binaries.
 *   requires_fcv_91,
 * ]
 */
import {describe, it, after} from "jstests/libs/mochalite.js";
import {
    assertDropCollection,
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    assertGetMoreFailsWithExpectedError,
    ChangeStreamTest,
    isChangeStreamPassthrough,
} from "jstests/libs/query/change_stream_util.js";

/**
 * Opens a change stream on 'testColl', generates an event, and returns that event's resume token.
 */
function getResumeToken(testColl, doc) {
    const csCursor = testColl.watch();
    assert.commandWorked(testColl.insert(doc));
    assert.soon(() => csCursor.hasNext(), "expecting a change stream event to resume from");
    const resumeToken = csCursor.next()._id;
    csCursor.close();
    return resumeToken;
}

/**
 * Asserts that 'csCursor' neither produces an event nor fails while the oplog keeps advancing.
 * 'advance' is called before each check to generate oplog entries which the stream filters out.
 */
function assertStreamStaysOpen(csCursor, advance) {
    for (let i = 0; i < 5; i++) {
        advance(i);
        assert.eq(false, csCursor.hasNext(), "the change stream produced an unexpected event");
    }
    csCursor.close();
}

describe("a resumed change stream and the high water mark of its underlying scan", function () {
    const kCollName = jsTestName();

    after(function () {
        assertDropCollection(db, kCollName);
    });

    // Resumes a change stream from an event which the resumed stream's filter excludes, and asserts
    // that the stream fails fast rather than waiting for an event to arrive.
    it("fails fast against the scan's high water mark", function () {
        const testColl = assertDropAndRecreateCollection(db, kCollName);
        const resumeToken = getResumeToken(testColl, {_id: 1});

        // This filter matches no event whatsoever, so the resumed stream never produces one. In
        // particular it excludes the resume point itself, which makes the stream unresumable.
        const resumedCursor = testColl.watch([{$match: {"documentKey._id": {$lt: -1000}}}], {
            resumeAfter: resumeToken,
            batchSize: 0,
        });

        // Generate oplog entries which the stream filters out, so that the high water mark advances
        // beyond the resume point without any event being produced.
        assert.commandWorked(testColl.insert([{_id: 2}, {_id: 3}, {_id: 4}]));

        // Without the high water mark check the stream would never fail, because it would never observe
        // an event which surpasses the resume point. The cursor's own database and collection name are
        // used because a passthrough may have upconverted the stream to a collectionless namespace.
        const err = assertGetMoreFailsWithExpectedError({
            db: resumedCursor._db,
            cursorId: resumedCursor.getId(),
            collName: resumedCursor._collName,
            expectedError: (res) => res.code === ErrorCodes.ChangeStreamFatalError,
            msg: "the change stream neither produced an event nor failed",
        });

        // Confirm the failure came from the high water mark check and not from the pre-existing path
        // where an observed event surpasses the resume token.
        assert(
            /has advanced to cluster time/.test(err.errmsg),
            "expected the failure to come from the high water mark check",
            {error: err.errmsg},
        );
    });

    // Resumes a change stream from an event which the resumed stream's filter retains, but which is followed by no further matching event, and asserts that the stream stays open no matter how far the high water mark advances past the resume point.
    it("stays open when the resume point was observed", function () {
        const testColl = assertDropAndRecreateCollection(db, kCollName);
        const cst = new ChangeStreamTest(db);
        try {
            // Generate the resume point with a change stream opened through the fixture, so that any
            // active passthrough is applied consistently to both the seed and the resumed stream.
            const seedCursor = cst.startWatchingChanges({
                pipeline: [{$changeStream: {}}],
                collection: testColl,
            });
            assert.commandWorked(testColl.insert({_id: 100}));
            const resumeToken = cst.getOneChange(seedCursor)._id;

            // This filter matches the resume point itself, so the stage observes the resume token,
            // but it matches nothing which follows it.
            const resumedCursor = cst.startWatchingChanges({
                pipeline: [
                    {$changeStream: {resumeAfter: resumeToken}},
                    {$match: {"documentKey._id": 100}},
                ],
                collection: testColl,
                aggregateOptions: {cursor: {batchSize: 0}},
            });

            // Having seen the resume token, the stage must not fail the stream however far the high
            // water mark advances; we are merely waiting for the event which follows the resume
            // point.
            for (let i = 0; i < 5; i++) {
                assert.commandWorked(testColl.insert({_id: 200 + i}));
                cst.assertNoChange(resumedCursor);
            }

            // The stream's resume token must also advance past the resume point, proving that the
            // stream observed it rather than merely staying open. On a sharded cluster the exposed
            // postBatchResumeToken is the minimum across shards, so it only moves past the resume
            // point once an idle shard emits a periodic noop.
            assert.soon(
                () => {
                    cst.assertNoChange(resumedCursor);
                    return bsonWoCompare(cst.getResumeToken(resumedCursor), resumeToken) > 0;
                },
                "the stream's resume token did not advance past the resume point",
                undefined,
                undefined,
                undefined,
                {resumeToken},
            );
        } finally {
            cst.cleanUp();
        }
    });

    // A whole-db or whole-cluster stream does not produce an "invalidate" event when a single
    // collection is dropped, so this case only applies to collection-level streams.
    if (!isChangeStreamPassthrough()) {
        // Resumes a change stream with 'startAfter' set to an invalidate token and asserts that the
        // stream stays open while the high water mark advances past it. The invalidate event is
        // reconstructed rather than read straight from the stream, so this exercises the case where
        // the resume point is only observed once the stage has already seen the oplog entry which
        // produced it.
        it("stays open when resuming after an invalidate", function () {
            const testColl = assertDropAndRecreateCollection(db, "startAfterInvalidate");
            assert.commandWorked(testColl.insert({_id: 1}));

            const csCursor = testColl.watch();
            assert(testColl.drop());

            let invalidateToken;
            assert.soon(() => {
                while (csCursor.hasNext()) {
                    const event = csCursor.next();
                    if (event.operationType === "invalidate") {
                        invalidateToken = event._id;
                        return true;
                    }
                }
                return false;
            }, "expecting an invalidate event to resume from");
            csCursor.close();

            const resumedCursor = testColl.watch([], {startAfter: invalidateToken, batchSize: 0});

            // Writes to an unrelated collection advance every shard's high water mark past the
            // invalidate without producing an event on this stream.
            const unrelatedColl = assertDropAndRecreateCollection(db, "unrelated");
            assertStreamStaysOpen(resumedCursor, (i) => {
                assert.commandWorked(unrelatedColl.insert({_id: i}));
            });
        });
    }
});
