/**
 * Tests that the change stream server status metrics are correctly reflected in the serverStatus
 * command output.
 *
 * @tags: [
 *   # TODO SERVER-122262: Remove the 'assumes_against_mongod_not_mongos' tag once the metrics are available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   change_stream_does_not_expect_txns,
 *   # Can be removed once last-lts and last-continuous are >= 9.0.
 *   requires_fcv_90,
 *   uses_change_streams,
 *   uses_parallel_shell,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");

// Helper object for retrieving change stream metrics from the 'serverStatus' command's output.
const ServerStatusMetrics = {
    getCsCursorTotalOpened: function () {
        return this.get("changeStreams.cursor.totalOpened");
    },

    getCsCursorLifespan: function () {
        return this.get("changeStreams.cursor.lifespan");
    },

    getCsCursorOpenTotal: function () {
        return this.get("changeStreams.cursor.open.total");
    },

    getCsCursorOpenPinned: function () {
        return this.get("changeStreams.cursor.open.pinned");
    },

    /**
     * Returns the value of the specified dot-separated path within serverStatus.metrics.
     */
    get: function (path) {
        const serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1, metrics: 1}));
        return path.split(".").reduce((obj, key) => obj[key], serverStatus?.metrics);
    },
};

describe("change stream cursor metrics in serverStatus", function () {
    before(function () {
        testColl.drop();
        assert.commandWorked(testColl.insert({_id: 1}));
    });

    beforeEach(function () {
        this.totalOpenedBefore = ServerStatusMetrics.getCsCursorTotalOpened();
        this.lifespanBefore = ServerStatusMetrics.getCsCursorLifespan();
        this.cursorOpenTotalBefore = ServerStatusMetrics.getCsCursorOpenTotal();
        this.cursorOpenPinnedBefore = ServerStatusMetrics.getCsCursorOpenPinned();
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        // Close any cursors left open by a test (e.g. on failure).
        this.cursorList.closeAll();
    });

    after(function () {
        testColl.drop();
    });

    it("changeStreams.cursor.totalOpened increases as change stream cursors are opened", function () {
        this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());
        assert.eq(
            this.totalOpenedBefore + this.cursorList.length(),
            ServerStatusMetrics.getCsCursorTotalOpened(),
            "totalOpened should increase by the number of opened change stream cursors",
        );
    });

    it("changeStreams.cursor.totalOpened does not decrease when change stream cursors are closed", function () {
        this.cursorList.push(testColl.watch(), testColl.watch());
        const afterOpen = ServerStatusMetrics.getCsCursorTotalOpened();

        this.cursorList.closeAll();

        // totalOpened is a counter — it must not decrease on close.
        assert.eq(afterOpen, ServerStatusMetrics.getCsCursorTotalOpened());
    });

    it("changeStreams.cursor.lifespan histogram is populated after a change stream cursor is closed", function () {
        this.cursorList.push(testColl.watch());
        // The histogram should be unchanged while the cursor is still open.
        assert.docEq(this.lifespanBefore, ServerStatusMetrics.getCsCursorLifespan());
        this.cursorList.closeAll();

        const lifespanHistogram = ServerStatusMetrics.getCsCursorLifespan();
        assert.gte(lifespanHistogram.average, 0, "lifespan average must be non-negative");
        assert.eq(
            this.lifespanBefore.count + 1,
            lifespanHistogram.count,
            "lifespan count should increase by 1 after closing a change stream cursor",
        );
    });

    it("changeStreams.cursor.open.total increases as change stream cursors are opened", function () {
        this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());
        // 'changeStreams.cursor.open.total' should increase, while 'changeStreams.cursor.open.pinned' remain same as before.
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned());
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal());
    });

    it("changeStreams.cursor.open.total decreases as change stream cursors are closed", function () {
        this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());

        this.cursorList.pop().close();
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal());
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned());

        this.cursorList.pop().close();
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal());
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned());

        this.cursorList.pop().close();
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal());
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned());
    });

    it("changeStreams.cursor.open.pinned increases while a getMore is in progress", function () {
        // Enable the failpoint that pauses getMore after the cursor is pinned but before the pin
        // is released.
        const fp = configureFailPoint(db, "getMoreHangAfterPinCursor");

        // In a parallel shell, open a change stream and call hasNext() to trigger a getMore, which
        // pins the cursor and hits the failpoint.
        const joinGetMore = startParallelShell(
            funWithArgs(
                function (dbName, collName) {
                    const csCursor = db.getSiblingDB(dbName).getCollection(collName).watch();
                    csCursor.hasNext();
                    csCursor.close();
                },
                testDB.getName(),
                testColl.getName(),
            ),
            db.getMongo().port,
        );

        // Wait until the parallel shell's getMore hits the failpoint (cursor is now pinned).
        fp.wait();

        // Assert the pinned metric has increased by 1 while the cursor is pinned.
        assert.eq(this.cursorOpenPinnedBefore + 1, ServerStatusMetrics.getCsCursorOpenPinned());

        // Release the failpoint to let the parallel shell's getMore complete.
        fp.off();

        // Wait for the parallel shell to finish.
        joinGetMore();

        // Assert the pinned metric has returned to the original value after the cursor is unpinned.
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned());
    });

    it("a regular (non-change-stream) cursor does not affect change stream metrics", function () {
        const regularCursorOpenBefore = ServerStatusMetrics.get("cursor.open.total");

        // Open a regular find cursor with batchSize:0. This returns an empty first batch, but
        // leaves the cursor open on the server, making it a live multi-batch cursor without it
        // being a change stream.
        const cursor = this.cursorList.push(
            new DBCommandCursor(
                testDB,
                assert.commandWorked(testDB.runCommand({find: testColl.getName(), filter: {}, batchSize: 0})),
                0,
            ),
        );
        assert.neq(cursor.getId(), 0, "unexpected empty cursorId");
        assert.eq(regularCursorOpenBefore + 1, ServerStatusMetrics.get("cursor.open.total"));

        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned());
        assert.eq(this.cursorOpenTotalBefore, ServerStatusMetrics.getCsCursorOpenTotal());
        assert.eq(this.totalOpenedBefore, ServerStatusMetrics.getCsCursorTotalOpened());
        assert.docEq(this.lifespanBefore, ServerStatusMetrics.getCsCursorLifespan());
    });
});
