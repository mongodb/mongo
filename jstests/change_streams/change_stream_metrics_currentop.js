/**
 * Tests that after a getMore on a change stream cursor returns a batch with change events, the
 * change stream cursor metrics are exposed in $currentOp output.
 *
 * @tags: [
 *   requires_getmore,
 *   # The test assumes that cursors are opened on the primary, which is not guaranteed when change streams
 *   # are opened on secondaries.
 *   assumes_no_implicit_cursor_exhaustion,
 *   assumes_read_preference_unchanged,
 *   requires_fcv_90
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {CursorList, getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {listIdleCursors} from "jstests/libs/query/change_stream_util.js";
import {TestDataModifyGuard} from "jstests/change_streams/change_stream_metrics_util.js";

describe("change stream cursor metrics in currentOp", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const adminDB = db.getSiblingDB("admin");
    const testColl = testDB.getCollection("test");

    const getClusterTimeFromInsert = (db) => {
        return assert.commandWorked(db.runCommand({insert: "unrelated_collection", documents: [{}]})).$clusterTime
            .clusterTime;
    };

    function idleCursorFilter(comment, cursor = null) {
        const filter = {"cursor.originatingCommand.comment": comment};
        if (cursor !== null) {
            filter["cursor.cursorId"] = cursor.getId();
        }
        return filter;
    }

    function getIdleCursor(comment, cursor = null) {
        const cursors = listIdleCursors(adminDB, idleCursorFilter(comment, cursor), {localOps: true});
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

    // Returns a high-water-mark resume token by opening a temporary change stream, advancing it,
    // and closing it — without leaving any cursor open.
    function getHwmToken() {
        const csCursor = testColl.watch();
        try {
            csCursor.hasNext();
            return csCursor.getResumeToken();
        } finally {
            csCursor.close();
        }
    }

    before(function () {
        // The test is incompatible with multi-router setup, as we're checking per-process metrics.
        this.testDataChange = new TestDataModifyGuard("pinToSingleMongos", true);
        assertDropAndRecreateCollection(testDB, testColl.getName());
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

    it("optime equals postBatchResumeToken clusterTime after empty getMore", function () {
        const comment = "optime equals postBatchResumeToken clusterTime after empty getMore";
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
        const comment = "changeStreams is not set for regular (non change stream) cursors";

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

    it("optime not bumped when startAtOperationTime is behind oplog tip", function () {
        const comment = "optime not bumped when startAtOperationTime is behind oplog tip";

        // Get a past timestamp to use as the change stream start point.
        const startAtTime = getClusterTime(testDB);

        // Advance the oplog past 'startAtTime' so that currentLatest > startAtTime.
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

    it("optime advances when unrelated oplog events are skipped", function () {
        const comment = "optime advances when unrelated oplog events are skipped";

        // Open a change stream scoped only to testColl.
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        // Insert into testColl so the cursor has an event to read and an optime to anchor to.
        assert.commandWorked(testColl.insert({_id: "ins1"}));
        assert.soon(() => cursor.hasNext());
        const event = cursor.next();

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

    for (const resumeOption of ["resumeAfter", "startAfter"]) {
        it(`optime initialized from ${resumeOption} token`, function () {
            const comment = `optime initialized from ${resumeOption} token`;
            const hwmToken = getHwmToken();
            let cursor = this.cursorList.push(
                testColl.watch([], {comment, [resumeOption]: hwmToken, cursor: {batchSize: 0}}),
            );
            const idleCursor = getIdleCursor(comment, cursor);

            const optime = idleCursor.cursor.changeStreams.optime;
            assert(optime instanceof Timestamp, "Expected changeStreams.optime to be a Timestamp", {
                idleCursor,
            });
            assert.eq(
                optime,
                decodeResumeToken(hwmToken).clusterTime,
                `changeStreams.optime must equal the clusterTime of the ${resumeOption} token`,
                {optime, resumeToken: hwmToken},
            );
        });
    }

    it("optime initialized from future startAtOperationTime", function () {
        // Uses a future timestamp to exercise the mongoS initial aggregate path before any oplog
        // entries at that time exist, verifying the PBRT is derived from startAtOperationTime
        // and not clamped to the current oplog tip.
        const comment = "optime initialized from future startAtOperationTime";
        const futureTimestamp = Timestamp(Math.floor(Date.now() / 1000) + 60, 0);
        let cursor = this.cursorList.push(
            testColl.watch([], {
                comment,
                startAtOperationTime: futureTimestamp,
                cursor: {batchSize: 0},
            }),
        );
        const idleCursor = getIdleCursor(comment, cursor);

        assert.eq(
            futureTimestamp,
            idleCursor.cursor.changeStreams.optime,
            "changeStreams.optime must equal startAtOperationTime before any getMore",
        );
    });

    it("optime initialized to cluster time with no start option", function () {
        const comment = "optime initialized to cluster time with no start option";

        // TODO SERVER-128391: Investigate why using 'getClusterTime()' from change_stream_util.js
        // does not work in disagg test suites.
        const currentClusterTime = getClusterTimeFromInsert(testDB);
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        const idleCursor = getIdleCursor(comment, cursor);

        jsTest.log.info("Important timestamps", {
            clusterTimeFromHello: getClusterTime(testDB),
            clusterTimeFromInsert: currentClusterTime,
            clusterTimeFromAdminDB: getClusterTime(testDB.getSiblingDB("admin")),
            changeStreamOptime: idleCursor.cursor.changeStreams.optime,
        });

        const optime = idleCursor.cursor.changeStreams.optime;
        assert(optime instanceof Timestamp, "Expected changeStreams.optime to be a timestamp", {idleCursor});
        assert.gte(
            optime,
            currentClusterTime,
            "Expected changeStreams.optime greater or equal to the cluster time snapshot",
            {idleCursor},
        );
    });

    it("optime advances monotonically across successive getMores", function () {
        const comment = "optime advances monotonically across successive getMores";
        let prevOptime = getClusterTimeFromInsert(testDB);
        const cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        for (let i = 0; i < 3; i++) {
            assert.commandWorked(testColl.insert({_id: `multi${i}`}));
            assert.soon(() => cursor.hasNext());
            const event = cursor.next();
            const idleCursor = getIdleCursor(comment, cursor);
            const optime = idleCursor.cursor.changeStreams.optime;
            assert(optime instanceof Timestamp, `Expected optime after getMore ${i}`, {idleCursor});
            assert.gte(optime, event.clusterTime, "Expected optime >= event clusterTime");
            assert.gt(optime, prevOptime, "Expected optime to advance between getMores");
            prevOptime = optime;
        }
    });
});
