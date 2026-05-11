/**
 * Tests that the change stream server status metrics are correctly reflected in the serverStatus
 * command output.
 *
 * @tags: [
 *   # The test assumes that serverStatus metrics are retrieved from the same instance that the
 *   # change streams are opened on, which is not guaranteed when change streams are opened on secondaries.
 *   assumes_read_preference_unchanged,
 *   change_stream_does_not_expect_txns,
 *   # Can be removed once last-lts and last-continuous are >= 9.0.
 *   requires_fcv_90,
 *   uses_change_streams,
 *   uses_parallel_shell,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";
import {ServerStatusMetrics, TestDataModifyGuard} from "jstests/change_streams/change_stream_metrics_util.js";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");

function retryOnUnaccountedCursor(fn) {
    const ctx = {};
    assert.retryNoExcept(
        () => {
            ctx.cursorList = new CursorList();
            try {
                fn.call(ctx);
            } finally {
                ctx.cursorList.closeAll();
            }
            return true;
        },
        "change stream server status gauge test failed after multiple retries",
        10 /*attempts*/,
        100 /*interval*/,
        {runHangAnalyzer: false},
    );
}

before(function () {
    assertDropAndRecreateCollection(testDB, testColl.getName());
    assert.commandWorked(testColl.insert({_id: 1}));
    // The test is incompatible with multi-router setup, as we're checking per-process metrics.
    this.testDataChange = new TestDataModifyGuard("pinToSingleMongos", true);
});

after(function () {
    assertDropCollection(testDB, testColl.getName());
    this.testDataChange.restore();
});

describe("change stream cursor counter metrics in serverStatus", function () {
    beforeEach(function () {
        const csMetrics = ServerStatusMetrics.getCsMetrics(db);
        this.totalOpenedBefore = csMetrics.cursor.totalOpened;
        this.lifespanBefore = csMetrics.cursor.lifespan;
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        // Close any cursors left open by a test (e.g. on failure).
        this.cursorList.closeAll();
    });

    it("changeStreams.cursor.totalOpened increases as change stream cursors are opened", function () {
        this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());
        assert.gte(
            ServerStatusMetrics.getCsMetrics(db).cursor.totalOpened,
            this.totalOpenedBefore + this.cursorList.length(),
            "totalOpened should increase by the number of opened change stream cursors",
        );
    });

    it("changeStreams.cursor.totalOpened does not decrease when change stream cursors are closed", function () {
        this.cursorList.push(testColl.watch(), testColl.watch());
        const afterOpen = ServerStatusMetrics.getCsMetrics(db).cursor.totalOpened;

        this.cursorList.closeAll();

        // totalOpened is a counter - it must not decrease on close.
        assert.gte(ServerStatusMetrics.getCsMetrics(db).cursor.totalOpened, afterOpen);
    });

    it("changeStreams.cursor.lifespan histogram is populated after a change stream cursor is closed", function () {
        this.cursorList.push(testColl.watch());
        this.cursorList.closeAll();

        assert.gte(
            ServerStatusMetrics.getCsMetrics(db).cursor.lifespan.totalCount,
            this.lifespanBefore.totalCount + 1,
            "lifespan totalCount should increase by 1 after closing a change stream cursor",
        );
    });
});

describe("change stream cursor open gauge metrics in serverStatus", function () {
    it("changeStreams.cursor.open.total increases as change stream cursors are opened", function () {
        retryOnUnaccountedCursor(function () {
            const csMetricsBefore = ServerStatusMetrics.getCsMetrics(db);

            this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());
            // 'changeStreams.cursor.open.total' should increase, while 'changeStreams.cursor.open.pinned' remain same as before.

            const csMetricsAfter = ServerStatusMetrics.getCsMetrics(db);
            assert.eq(csMetricsBefore.cursor.open.pinned, csMetricsAfter.cursor.open.pinned);
            assert.eq(csMetricsBefore.cursor.open.total + this.cursorList.length(), csMetricsAfter.cursor.open.total);
        });
    });

    it("changeStreams.cursor.open.total decreases as change stream cursors are closed", function () {
        retryOnUnaccountedCursor(function () {
            const csMetricsBefore = ServerStatusMetrics.getCsMetrics(db);

            this.cursorList.push(testColl.watch(), testColl.watch(), testColl.watch());
            assert.eq(
                ServerStatusMetrics.getCsMetrics(db).cursor.open.total,
                csMetricsBefore.cursor.open.total + this.cursorList.length(),
            );

            this.cursorList.closeAll();
            const csMetricsAfter = ServerStatusMetrics.getCsMetrics(db);
            assert.eq(csMetricsBefore.cursor.open.total, csMetricsAfter.cursor.open.total);
            assert.eq(csMetricsBefore.cursor.open.pinned, csMetricsAfter.cursor.open.pinned);
        });
    });

    it("changeStreams.cursor.open.pinned increases while a getMore is in progress", function () {
        retryOnUnaccountedCursor(function () {
            const csMetricsBefore = ServerStatusMetrics.getCsMetrics(db);

            // Enable the failpoint that pauses getMore after the cursor is pinned but before the pin
            // is released.
            const fp = configureFailPoint(db, "waitAfterPinningCursorBeforeGetMoreBatch");
            let joinGetMore;
            try {
                // In a parallel shell, open a change stream and call hasNext() to trigger a getMore, which
                // pins the cursor and hits the failpoint.
                joinGetMore = startParallelShell(
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
                assert.eq(
                    csMetricsBefore.cursor.open.pinned + 1,
                    ServerStatusMetrics.getCsMetrics(db).cursor.open.pinned,
                );
            } finally {
                // Release the failpoint to let the parallel shell's getMore complete.
                fp.off();
                if (joinGetMore) {
                    // Wait for the parallel shell to finish.
                    joinGetMore();
                }
            }

            // Assert the pinned metric has returned to the original value after the cursor is unpinned.
            assert.eq(csMetricsBefore.cursor.open.pinned, ServerStatusMetrics.getCsMetrics(db).cursor.open.pinned);
        });
    });

    it("a regular (non-change-stream) cursor does not affect change stream metrics", function () {
        retryOnUnaccountedCursor(function () {
            const csMetricsBefore = ServerStatusMetrics.getCsMetrics(db);
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
            assert.eq(csMetricsBefore.cursor.open.pinned, csMetricsAfter.cursor.open.pinned);
            assert.eq(csMetricsBefore.cursor.open.total, csMetricsAfter.cursor.open.total);
            assert.eq(csMetricsBefore.cursor.totalOpened, csMetricsAfter.cursor.totalOpened);
        });
    });
});
