/**
 * Tests that after a getMore on a change stream cursor returns a batch with change events, the
 * cursor's changeStreams.optime is exposed in $currentOp idle cursor output.
 *
 * @tags: [
 *   requires_getmore,
 *   # TODO SERVER-123932: Remove the 'assumes_against_mongod_not_mongos' tag once the metrics are available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_cursor_exhaustion,
 *   requires_fcv_90
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";
import {listIdleCursors} from "jstests/libs/query/change_stream_util.js";

describe("change stream cursor metrics in currentOp", function () {
    let originalPinToSingleMongos;

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

    before(function () {
        testColl.drop();
        assert.commandWorked(testColl.insert({_id: 1}));
    });

    beforeEach(function () {
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        // Close any cursors left open by a test (e.g. on failure).
        this.cursorList.closeAll();
    });

    after(function () {
        testColl.drop();
        testDB.getCollection("other_unrelated").drop();
        TestData.pinToSingleMongos = originalPinToSingleMongos;
    });

    it("changeStreams.optime is set to the high-watermark when no events have been returned", function () {
        const comment = "change_stream_cursor hwm no events";
        let cursor = testColl.watch([], {comment, cursor: {batchSize: 0}});
        this.cursorList.push(cursor);

        // Issue a getMore that returns immediately with no events by using a 1ms timeout.
        // For a tailable awaitData cursor, maxTimeMS controls how long to wait for new data;
        // when it expires the server returns an empty batch rather than an error.
        const getMoreRes = assert.commandWorked(
            cursor._db.runCommand({
                getMore: cursor.getId(),
                collection: cursor._collName,
                maxTimeMS: 1,
            }),
        );

        assert.eq(0, getMoreRes.cursor.nextBatch.length, "Expected an empty batch");

        const cursors = listIdleCursors(adminDB, idleCursorFilter(comment, cursor));
        assert.eq(1, cursors.length, "Expected exactly one idle change stream cursor");

        const optime = cursors[0].cursor.changeStreams.optime;
        assert(optime instanceof Timestamp, "Expected changeStreams.optime to be a Timestamp", {
            idleCursor: cursors[0],
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

        const findRes = assert.commandWorked(
            testDB.runCommand({
                find: testColl.getName(),
                batchSize: 0,
                comment: comment,
            }),
        );
        const cursorId = findRes.cursor.id;
        assert.neq(NumberLong(0), cursorId, "Expected cursor to remain open after batchSize:0 find");
        this.cursorList.push(new DBCommandCursor(testDB, findRes));

        const cursors = listIdleCursors(adminDB, idleCursorFilter(comment));
        assert.eq(1, cursors.length, "Expected exactly one idle cursor");
        const cursor = cursors[0].cursor;
        assert.eq(null, cursor?.changeStreams, "Expected changeStreams to be absent for a regular cursor", {cursor});
    });

    it("changeStreams.optime is at least the clusterTime of the last returned event", function () {
        const comment = "change_stream_cursor current time";
        let cursor = testColl.watch([], {comment, cursor: {batchSize: 0}});

        this.cursorList.push(cursor);
        assert.commandWorked(testColl.insert({_id: 2}));

        // read from the cursor
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();
        const cursors = listIdleCursors(adminDB, idleCursorFilter(comment, cursor));

        assert.eq(1, cursors.length, "Expected exactly one idle change stream cursor");
        const optime = cursors[0].cursor.changeStreams.optime;
        assert(optime instanceof Timestamp, "Expected changeStreams.optime to be a Timestamp", {
            idleCursor: cursors[0],
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
        let cursor = testColl.watch([], {
            comment,
            startAtOperationTime: startAtTime,
            cursor: {batchSize: 0},
        });
        this.cursorList.push(cursor);

        const cursors = listIdleCursors(adminDB, idleCursorFilter(comment, cursor));
        assert.eq(1, cursors.length, "Expected exactly one idle change stream cursor");

        const optime = cursors[0].cursor.changeStreams.optime;
        assert(
            optime instanceof Timestamp,
            "Expected changeStreams.optime to be a Timestamp even before the first getMore",
            {idleCursor: cursors[0]},
        );
        assert.eq(
            startAtTime,
            optime,
            "Expected changeStreams.optime to equal startAtOperationTime before any getMore",
            {startAtTime, optime},
        );
    });

    it("changeStreams.optime advances when unrelated oplog events are skipped", function () {
        const comment = "change_stream_cursor optime advances unrelated event";

        // Open a change stream scoped only to testColl.
        let cursor = testColl.watch([], {comment, cursor: {batchSize: 0}});
        this.cursorList.push(cursor);

        // Insert into testColl so the cursor has an event to read and an optime to anchor to.
        assert.commandWorked(testColl.insert({_id: "ins1"}));
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();

        // Filter by cursor ID to avoid picking up cursors from concurrent test instances
        // (e.g. burn_in runs the same test in parallel with identical comment strings).
        const cursorIdFilter = idleCursorFilter(comment, cursor);
        const cursorsAfterFirst = listIdleCursors(adminDB, cursorIdFilter);
        assert.eq(1, cursorsAfterFirst.length, "Expected exactly one idle change stream cursor");
        const optimeAfterFirstEvent = cursorsAfterFirst[0].cursor.changeStreams.optime;
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
        const getMoreRes = assert.commandWorked(
            cursor._db.runCommand({
                getMore: cursor.getId(),
                collection: cursor._collName,
                maxTimeMS: 1,
            }),
        );
        assert.eq(0, getMoreRes.cursor.nextBatch.length, "Expected empty batch after unrelated write");

        const cursorsAfterUnrelated = listIdleCursors(adminDB, cursorIdFilter);
        assert.eq(
            1,
            cursorsAfterUnrelated.length,
            "Expected exactly one idle change stream cursor after unrelated write",
        );
        const optimeAfterUnrelated = cursorsAfterUnrelated[0].cursor.changeStreams.optime;
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
