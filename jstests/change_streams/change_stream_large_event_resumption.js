/**
 * Tests that a change stream consumer can recover from a BSONObjectTooLarge error by resuming from
 * the previous event with fullDocument projected out, and then resume with full documents
 * afterwards. Runs on both replica sets and sharded clusters.
 *
 * @tags: [
 *   # Requires an unsharded collection so that there is a deterministic sequence of events produced
 *   # by a single shard. If the collection were sharded, the next events may be fetched concurrently
 *   # from multiple shards, which could make the result fetching order non-deterministic.
 *   assumes_unsharded_collection,
 *   change_stream_does_not_expect_txns,
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropCollection,
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {configureFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";

describe("Change stream large event resumption", function () {
    const kDbName = jsTestName();
    const kCollName = "coll";
    const kShardGetMoreBatchSizeFailPoint = "overrideBatchSizeForShardGetMore";

    let coll;
    let startTime;

    const insertDocument = (doc) => {
        assert.commandWorked(coll.insert(doc));
    };

    const updateDocument = (lookup, update) => {
        assert.commandWorked(coll.update(lookup, update));
    };

    const makeLargeEventId = (value) => {
        assert(value < 10);
        return "large" + value;
    };

    // Events to be created and observed.
    const generateEvents = () => {
        const kMaxBsonSize = 16 * 1024 * 1024;
        const kLargeStringSize =
            kMaxBsonSize - bsonsize({_id: makeLargeEventId(1), value: "x"}) + 1;

        return [
            {
                id: makeLargeEventId(1),
                type: "insert",
                tooLarge: false,
                execute: () => {
                    // Build a large document whose size is exactly the maximum user BSON size.
                    insertDocument({_id: makeLargeEventId(1), value: "x".repeat(kLargeStringSize)});
                },
            },
            {
                id: 1,
                type: "insert",
                tooLarge: false,
                execute: () => {
                    insertDocument({_id: 1});
                },
            },
            {
                id: makeLargeEventId(1),
                type: "update",
                tooLarge: true,
                execute: () => {
                    // The change event for an update both reports the new large field in
                    // updateDescription and looks up the post-image as fullDocument, and thus will
                    // far exceed the server's internal BSON size limit.
                    updateDocument(
                        {_id: makeLargeEventId(1)},
                        {$set: {value: "y".repeat(kLargeStringSize)}},
                    );
                },
            },
            {
                id: 2,
                type: "insert",
                tooLarge: false,
                execute: () => {
                    insertDocument({_id: 2});
                },
            },
            {
                id: 3,
                type: "insert",
                tooLarge: false,
                execute: () => {
                    insertDocument({_id: 3});
                },
            },
            {
                id: makeLargeEventId(2),
                type: "insert",
                tooLarge: false,
                execute: () => {
                    // Build a large document whose size is exactly the maximum user BSON size.
                    insertDocument({_id: makeLargeEventId(2), value: "x".repeat(kLargeStringSize)});
                },
            },
            {
                id: makeLargeEventId(2),
                type: "update",
                tooLarge: true,
                execute: () => {
                    // The change event for an update both reports the new large field in
                    // updateDescription and looks up the post-image as fullDocument, and thus will
                    // far exceed the server's internal BSON size limit.
                    updateDocument(
                        {_id: makeLargeEventId(2)},
                        {$set: {value: "y".repeat(kLargeStringSize)}},
                    );
                },
            },
            {
                id: 4,
                type: "insert",
                tooLarge: false,
                execute: () => {
                    insertDocument({_id: 4});
                },
            },
        ];
    };

    // In a sharded cluster, pin the batch size of the getMore requests that each mongos sends to
    // the shards, so that each request returns at most one change event. Without this, the
    // AsyncResultsMerger does not forward the client's batch size to the shards and overfetches,
    // causing the batch containing the intentionally-too-large event to be serialized and fail with
    // BSONObjectTooLarge before the preceding small event can be returned. This is a no-op on
    // replica sets, where the batch size from the client's getMore is honored directly.
    const setShardGetMoreBatchSizeFailPoint = (mode) => {
        if (!FixtureHelpers.isMongos(db)) {
            return;
        }

        configureFailPointForAllShardsAndMongos({
            conn: db.getMongo(),
            failPointName: kShardGetMoreBatchSizeFailPoint,
            data: {batchSize: 1},
            failPointMode: mode,
        });
    };

    before(function () {
        coll = assertDropAndRecreateCollection(db, kCollName);

        // Note start time and create change events.
        startTime = getClusterTime(db);

        const events = generateEvents();
        for (const e of events) {
            e.execute();
        }

        setShardGetMoreBatchSizeFailPoint("alwaysOn");
    });

    after(function () {
        setShardGetMoreBatchSizeFailPoint("off");
        assertDropCollection(db, kCollName);
    });

    it("projects out fullDocument after BSONObjectTooLarge, then resumes full documents", function () {
        let cursor = null;

        const openCursor = (pipeline, options) => {
            if (cursor) {
                cursor.close();
            }
            // batchSize: 1 keeps the client-side getMores bounded to a single event. On a sharded
            // cluster the to-shard requests are bounded separately via the
            // 'overrideBatchSizeForShardGetMore' failpoint enabled in 'before'.
            cursor = coll.watch(pipeline, Object.assign({batchSize: 1}, options));
        };

        // Fetches the next event and checks it against 'expectedEvent', verifying the presence or
        // absence of 'fullDocument'.
        const fetchAndCheckEvent = (expectedEvent, expectFullDocument) => {
            assert.soon(() => cursor.hasNext(), "timed out waiting for event", {
                expectedEvent,
                expectFullDocument,
            });
            const e = cursor.next();
            assert.eq(expectedEvent.id, e.documentKey._id, {expectedEvent, e});
            assert.eq(expectedEvent.type, e.operationType, {expectedEvent, e});
            assert.eq(
                expectFullDocument,
                e.hasOwnProperty("fullDocument"),
                "unexpected fullDocument presence",
                {expectedEvent, e},
            );
        };

        const events = generateEvents();
        let lastToken;
        let fullDocumentMode = true;

        openCursor([], {startAtOperationTime: startTime, fullDocument: "updateLookup"});

        try {
            for (let i = 0; i < events.length; ++i) {
                const expectedEvent = events[i];

                if (fullDocumentMode && expectedEvent.tooLarge) {
                    // With fullDocument enabled, fetching this event must fail with
                    // BSONObjectTooLarge.
                    try {
                        assert.soon(
                            () => cursor.hasNext(),
                            "timed out waiting for the too-large event",
                            {expectedEvent},
                        );
                        cursor.next();
                        assert(false, "too-large event was returned without error", {
                            expectedEvent,
                        });
                    } catch (e) {
                        assert.eq(ErrorCodes.BSONObjectTooLarge, e.code, {e});
                    }

                    // Resume with fullDocument projected out so that the too-large event can be
                    // delivered.
                    openCursor([{$project: {fullDocument: 0}}], {resumeAfter: lastToken});
                    fullDocumentMode = false;
                }

                // Deliver 'expectedEvent' in the current mode.
                fetchAndCheckEvent(expectedEvent, fullDocumentMode);
                lastToken = cursor.getResumeToken();

                if (!fullDocumentMode) {
                    // The resume-token event is enriched with its full document before the stream
                    // can swallow it, so re-enabling fullDocument using this event's own token
                    // would recompute the too-large event and fail again. Consume one more event
                    // without fullDocument to advance the resume token past the too-large event,
                    // then switch back to full documents.
                    assert(i + 1 < events.length, "expected an event after the too-large event", {
                        expectedEvent,
                    });
                    const nextEvent = events[++i];
                    fetchAndCheckEvent(nextEvent, false /* expectFullDocument */);
                    lastToken = cursor.getResumeToken();

                    fullDocumentMode = true;
                    openCursor([], {resumeAfter: lastToken, fullDocument: "updateLookup"});
                }
            }
        } finally {
            cursor.close();
        }
    });
});
