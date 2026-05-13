/**
 * Tests that after a getMore on a change stream cursor returns a batch with change events, the
 * cursor's changeStreams.optime is exposed in $currentOp idle cursor output.
 *
 * @tags: [
 *   requires_getmore,
 *   # TODO SERVER-123932: Remove the 'assumes_against_mongod_not_mongos' tag once the metrics are available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # The test assumes that cursors are opened on the primary, which is not guaranteed when change streams
 *   # are opened on secondaries.
 *   assumes_no_implicit_cursor_exhaustion,
 *   assumes_read_preference_unchanged,
 *   requires_fcv_90
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";
import {listIdleCursors} from "jstests/libs/query/change_stream_util.js";
import {TestDataModifyGuard} from "jstests/change_streams/change_stream_metrics_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";

describe("change stream cursor metrics in currentOp", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const adminDB = db.getSiblingDB("admin");
    const testColl = testDB.getCollection("test");

    function idleCursorFilter(comment, cursor = null) {
        const filter = {"cursor.originatingCommand.comment": comment};
        if (cursor !== null) {
            filter["cursor.cursorId"] = cursor.getId();
        }
        return filter;
    }

    function getIdleCursor(comment, cursor = null) {
        const cursors = listIdleCursors(adminDB, idleCursorFilter(comment, cursor));
        assert.eq(1, cursors.length, "Expected exactly one idle change stream cursor");
        return cursors[0];
    }

    function runGetMore(cursor) {
        return assert.commandWorked(
            cursor._db.runCommand({
                getMore: cursor.getId(),
                collection: cursor._collName,
                maxTimeMS: 1,
            }),
        );
    }

    before(function () {
        assertDropAndRecreateCollection(testDB, testColl.getName());
        assert.commandWorked(testColl.insert({_id: 1}));
        // The test is incompatible with multi-router setup, as we're checking per-process metrics.
        this.testDataChange = new TestDataModifyGuard("pinToSingleMongos", true);
    });

    beforeEach(function () {
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        // Close any cursors left open by a test (e.g. on failure).
        this.cursorList.closeAll();
    });

    after(function () {
        assertDropCollection(testDB, testColl.getName());
        assertDropCollection(testDB, "other_unrelated");
        this.testDataChange.restore();
    });

    it("changeStreams.optime is set to the high-watermark when no events have been returned", function () {
        const comment = "change_stream_cursor hwm no events";
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        // Issue a getMore that returns immediately with no events by using a 1ms timeout.
        // For a tailable awaitData cursor, maxTimeMS controls how long to wait for new data;
        // when it expires the server returns an empty batch rather than an error.
        const getMoreRes = runGetMore(cursor);

        assert.eq(0, getMoreRes.cursor.nextBatch.length, "Expected an empty batch");

        const idleCursor = getIdleCursor(comment, cursor);

        const optime = idleCursor.cursor.changeStreams.optime;
        assert(optime instanceof Timestamp, "Expected changeStreams.optime to be a Timestamp", {
            idleCursor,
        });
        // Verify the optime equals the clusterTime encoded in the postBatchResumeToken.
        const postBatchResumeToken = getMoreRes.cursor.postBatchResumeToken;
        assert(postBatchResumeToken, "Expected a postBatchResumeToken in the getMore response", {getMoreRes});
        assert.eq(
            optime,
            decodeResumeToken(postBatchResumeToken).clusterTime,
            "changeStreams.optime must equal the clusterTime encoded in the postBatchResumeToken",
            {optime, postBatchResumeToken},
        );
    });

    it("changeStreams is not set for regular (non change stream) cursors", function () {
        const comment = "regular_cursor_no_optime";

        const findCursor = this.cursorList.push(
            new DBCommandCursor(
                testDB,
                assert.commandWorked(
                    testDB.runCommand({
                        find: testColl.getName(),
                        batchSize: 0,
                        comment: comment,
                    }),
                ),
                0,
            ),
        );
        assert.neq(NumberLong(0), findCursor.getId(), "Expected cursor to remain open after batchSize:0 find");

        const idleCursor = getIdleCursor(comment);

        assert.eq(null, idleCursor?.changeStreams, "Expected changeStreams to be absent for a regular cursor", {
            idleCursor,
        });
    });

    it("changeStreams.optime is at least the clusterTime of the last returned event", function () {
        const comment = "change_stream_cursor current time";
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        assert.commandWorked(testColl.insert({_id: 2}));

        // read from the cursor
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();

        const idleCursor = getIdleCursor(comment, cursor);

        const optime = idleCursor.cursor.changeStreams.optime;
        assert(optime instanceof Timestamp, "Expected changeStreams.optime to be a Timestamp", {
            idleCursor,
        });
        // The optime is a high-watermark: it equals the last event's clusterTime when no
        // additional oplog entries were scanned in the same getMore, but may be higher if
        // concurrent operations wrote oplog entries that the cursor scanned past without matching.
        assert.gte(optime, event.clusterTime, "Expected changeStreams.optime to be >= the event's clusterTime");
    });

    it("changeStreams.optime is the initial HWM before any getMore is issued", function () {
        const comment = "change_stream_cursor no getMore initial hwm";

        // Get a past timestamp to use as the change stream start point.
        const anchorRes = assert.commandWorked(
            testDB.runCommand({
                insert: testColl.getName(),
                documents: [{_id: "anchor_no_getMore"}],
            }),
        );
        const startAtTime = anchorRes.operationTime;
        assert(startAtTime instanceof Timestamp, "Expected operationTime from insert", {anchorRes});

        // Open the change stream starting at the past timestamp, with batchSize:0, and issue no
        // getMores. The plan executor initializes _latestOplogTimestamp from the initial post-batch
        // resume token at construction time, so $currentOp should reflect it immediately.
        let cursor = this.cursorList.push(
            testColl.watch([], {
                comment,
                startAtOperationTime: startAtTime,
                cursor: {batchSize: 0},
            }),
        );

        const idleCursor = getIdleCursor(comment, cursor);

        assert.eq(
            startAtTime,
            idleCursor.cursor.changeStreams.optime,
            "Expected changeStreams.optime to equal startAtOperationTime before any getMore",
        );
    });

    it("changeStreams.optime equals startAtOperationTime even when it is behind the current oplog tip", function () {
        const comment = "change_stream_cursor past startAtOperationTime no getMore";

        // Record a past timestamp from an anchor insert.
        const anchorRes = assert.commandWorked(
            testDB.runCommand({
                insert: testColl.getName(),
                documents: [{_id: "anchor_past_start"}],
            }),
        );
        const startAtTime = anchorRes.operationTime;
        assert(startAtTime instanceof Timestamp, "Expected operationTime from insert", {anchorRes});

        // Advance the oplog past startAtTime so that currentLatest > startAtTime.
        assert.commandWorked(testColl.insert({_id: "advance_oplog_past_start"}));

        // Open the change stream with a startAtOperationTime that is clearly behind the current
        // oplog tip. Issue no getMores. The reported optime must equal startAtOperationTime exactly
        // — it must not be silently bumped up to the current oplog tip.
        let cursor = this.cursorList.push(
            testColl.watch([], {
                comment,
                startAtOperationTime: startAtTime,
                cursor: {batchSize: 0},
            }),
        );

        const idleCursor = getIdleCursor(comment, cursor);

        assert.eq(
            startAtTime,
            idleCursor.cursor.changeStreams.optime,
            "changeStreams.optime must equal startAtOperationTime, not the current oplog tip",
        );
    });

    it("changeStreams.optime advances when unrelated oplog events are skipped", function () {
        const comment = "change_stream_cursor optime advances unrelated event";

        // Open a change stream scoped only to testColl.
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        // Insert into testColl so the cursor has an event to read and an optime to anchor to.
        assert.commandWorked(testColl.insert({_id: "ins1"}));
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();

        // Filter by cursor ID to avoid picking up cursors from concurrent test instances
        // (e.g. burn_in runs the same test in parallel with identical comment strings).
        const cursorAfterFirst = getIdleCursor(comment, cursor);

        const optimeAfterFirstEvent = cursorAfterFirst.cursor.changeStreams.optime;
        assert(
            optimeAfterFirstEvent instanceof Timestamp,
            "Expected changeStreams.optime to be a Timestamp after reading an event",
            {optimeAfterFirstEvent},
        );
        assert.gte(
            optimeAfterFirstEvent,
            event.clusterTime,
            "Expected changeStreams.optime to be >= the first event's clusterTime",
        );

        // Write to a different collection — this enters the oplog but does not match the
        // change stream filter, so the cursor will scan past it without returning an event.
        // Sleep briefly to guard against clock skew causing the unrelated write's timestamp
        // to be <= the first event's timestamp.
        sleep(500);
        const otherColl = testDB.getCollection("other_unrelated");
        assert.commandWorked(otherColl.insert({_id: "unrelated"}));

        // getMore with a short timeout: the unrelated write means the cursor will advance
        // through the oplog but produce an empty batch.
        const getMoreRes = runGetMore(cursor);
        assert.eq(0, getMoreRes.cursor.nextBatch.length, "Expected empty batch after unrelated write");

        const cursorAfterUnrelated = getIdleCursor(comment, cursor);
        const optimeAfterUnrelated = cursorAfterUnrelated.cursor.changeStreams.optime;
        assert(
            optimeAfterUnrelated instanceof Timestamp,
            "Expected changeStreams.optime to remain a Timestamp after unrelated write",
            {optimeAfterUnrelated},
        );
        assert.gte(
            0,
            timestampCmp(optimeAfterFirstEvent, optimeAfterUnrelated),
            "Expected changeStreams.optime to advance after an unrelated oplog event was scanned",
            {before: optimeAfterFirstEvent, after: optimeAfterUnrelated},
        );
    });
});
