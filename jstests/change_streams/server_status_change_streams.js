/**
 * Tests that the change stream server status metrics are correctly reflected in the serverStatus
 * command output.
 *
 * @tags: [
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
import {ServerStatusMetrics} from "jstests/change_streams/change_stream_metrics_util.js";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");

describe("change stream cursor metrics in serverStatus", function () {
    let originalPinToSingleMongos;

    before(function () {
        testColl.drop();
        assert.commandWorked(testColl.insert({_id: 1}));
        // The test is incompatible with multirouter setup, as we're checking per-process metrics.
        originalPinToSingleMongos = TestData.pinToSingleMongos;
        TestData.pinToSingleMongos = true;
    });

    beforeEach(function () {
        const csMetrics = ServerStatusMetrics.getCsMetrics(db);
        this.totalOpenedBefore = csMetrics.cursor.totalOpened;
        this.lifespanBefore = csMetrics.cursor.lifespan;
        this.cursorOpenTotalBefore = csMetrics.cursor.open.total;
        this.cursorOpenPinnedBefore = csMetrics.cursor.open.pinned;
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        // Close any cursors left open by a test (e.g. on failure).
        this.cursorList.closeAll();
    });

    after(function () {
        testColl.drop();
        TestData.pinToSingleMongos = originalPinToSingleMongos;
    });

    it("changeStreams.cursor.totalOpened increases as change stream cursors are opened", function () {
        this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());
        assert.eq(
            this.totalOpenedBefore + this.cursorList.length(),
            ServerStatusMetrics.getCsCursorTotalOpened(db),
            "totalOpened should increase by the number of opened change stream cursors",
        );
    });

    it("changeStreams.cursor.totalOpened does not decrease when change stream cursors are closed", function () {
        this.cursorList.push(testColl.watch(), testColl.watch());
        const afterOpen = ServerStatusMetrics.getCsCursorTotalOpened(db);

        this.cursorList.closeAll();

        // totalOpened is a counter - it must not decrease on close.
        assert.eq(afterOpen, ServerStatusMetrics.getCsCursorTotalOpened(db));
    });

    it("changeStreams.cursor.lifespan histogram is populated after a change stream cursor is closed", function () {
        this.cursorList.push(testColl.watch());
        // The histogram should be unchanged while the cursor is still open.
        assert.docEq(this.lifespanBefore, ServerStatusMetrics.getCsCursorLifespan(db));
        this.cursorList.closeAll();

        const lifespanHistogram = ServerStatusMetrics.getCsCursorLifespan(db);
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
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned(db));
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal(db));
    });

    it("changeStreams.cursor.open.total decreases as change stream cursors are closed", function () {
        this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());

        this.cursorList.pop().close();
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal(db));
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned(db));

        this.cursorList.pop().close();
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal(db));
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned(db));

        this.cursorList.pop().close();
        assert.eq(this.cursorOpenTotalBefore + this.cursorList.length(), ServerStatusMetrics.getCsCursorOpenTotal(db));
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned(db));
    });

    it("changeStreams.cursor.open.pinned increases while a getMore is in progress", function () {
        // Enable the failpoint that pauses getMore after the cursor is pinned but before the pin
        // is released.
        const fp = configureFailPoint(db, "waitAfterPinningCursorBeforeGetMoreBatch");

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
        assert.eq(this.cursorOpenPinnedBefore + 1, ServerStatusMetrics.getCsCursorOpenPinned(db));

        // Release the failpoint to let the parallel shell's getMore complete.
        fp.off();

        // Wait for the parallel shell to finish.
        joinGetMore();

        // Assert the pinned metric has returned to the original value after the cursor is unpinned.
        assert.eq(this.cursorOpenPinnedBefore, ServerStatusMetrics.getCsCursorOpenPinned(db));
    });

    it("a regular (non-change-stream) cursor does not affect change stream metrics", function () {
        const regularCursorOpenBefore = ServerStatusMetrics.getSsMetrics(db).cursor.open.total;

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
        const regularCursorOpenAfter = ServerStatusMetrics.getSsMetrics(db).cursor.open.total;

        assert.eq(regularCursorOpenBefore + 1, regularCursorOpenAfter);

        const csMetricsAfter = ServerStatusMetrics.getCsMetrics(db);
        assert.eq(this.cursorOpenPinnedBefore, csMetricsAfter.cursor.open.pinned);
        assert.eq(this.cursorOpenTotalBefore, csMetricsAfter.cursor.open.total);
        assert.eq(this.totalOpenedBefore, csMetricsAfter.cursor.totalOpened);
        assert.docEq(this.lifespanBefore, csMetricsAfter.cursor.lifespan);
    });
});
