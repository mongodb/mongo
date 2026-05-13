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
 *   assumes_write_concern_unchanged,
 *   requires_fcv_90,
 *   requires_profiling,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";
import {TestDataModifyGuard} from "jstests/change_streams/change_stream_metrics_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";

describe("change stream cursor metrics profiler / slow query output", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const testColl = testDB.getCollection("test");

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

    it("changeStreams.optime appears in the profiler output after a getMore", function () {
        const comment = "change_stream_cursor profiler_optime";
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        // Enable profiling at level 1 with slowms: 0 so every operation is profiled.
        assert.commandWorked(testDB.runCommand({profile: 1, filter: {"originatingCommand.comment": comment}}));

        try {
            runGetMore(cursor);

            let profileEntry;
            assert.soon(() => {
                profileEntry = testDB.system.profile.findOne({"originatingCommand.comment": comment});
                return profileEntry !== null;
            }, "Expected a profiler entry for the change stream getMore");

            assert(
                profileEntry.changeStreams?.optime,
                "Expected changeStreams.optime to be present in the profiler entry",
                {profileEntry},
            );
        } finally {
            assert.commandWorked(testDB.runCommand({profile: 0, filter: "unset"}));
        }
    });

    it("changeStreamMetrics.optime appears in the slow query log after a getMore with events", function () {
        const comment = "change_stream_cursor slow_query_log_optime";
        let cursor = this.cursorList.push(testColl.watch([], {comment, cursor: {batchSize: 0}}));

        // Log all operations, including the getMore that read the event above.
        const originalSlowMs = assert.commandWorked(db.adminCommand({profile: -1})).slowms;
        assert.commandWorked(db.adminCommand({profile: 0, slowms: 0}));

        try {
            // Re-issue a getMore so it gets captured with slowms: 0 in effect.
            runGetMore(cursor);

            let logLine;
            assert.soon(() => {
                const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
                logLine = findMatchingLogLine(globalLog.log, {
                    msg: "Slow query",
                    comment,
                });
                return logLine !== null;
            }, "Expected a 'Slow query' log entry with 'changeStreams' field");

            const parsedLogLine = JSON.parse(logLine);
            assert(
                parsedLogLine.attr.changeStreams?.optime,
                "Expected changeStreams.optime to be present in slow query log",
                {
                    parsedLogLine,
                },
            );
        } finally {
            assert.commandWorked(db.adminCommand({profile: 0, slowms: originalSlowMs}));
        }
    });
});
