/**
 * Tests that the change stream server status metrics are correctly reflected in the serverStatus
 * command output.
 *
 * @tags: [
 *   # TODO SERVER-122262: Remove the 'assumes_against_mongod_not_mongos' tag once the metrics are available on mongos.
 *   assumes_against_mongod_not_mongos,
 *   # Can be removed once last-lts and last-continuous are >= 9.0.
 *   requires_fcv_90,
 *   uses_change_streams,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");

// Helper object for retrieving change stream metrics from the 'serverStatus' command's output.
const ServerStatusMetrics = {
    /**
     * Returns the current value of the named change stream OTel metric from serverStatus.
     * 'suffix' is the unit string appended to the metric name (e.g. "cursors", "microseconds").
     */
    _getCSMetric: function (name, suffix) {
        const ss = assert.commandWorked(db.adminCommand({serverStatus: 1, otelMetrics: 1}));
        return ss?.otelMetrics["change_streams." + name + "_" + suffix];
    },

    getTotalOpened: function () {
        return this._getCSMetric("cursor.total_opened", "cursors");
    },

    getLifespan: function () {
        return this._getCSMetric("cursor.lifespan", "microseconds");
    },

    /**
     * Returns the value of the specified dot-separated path within serverStatus.metrics.
     */
    get: function (path) {
        const ss = assert.commandWorked(db.adminCommand({serverStatus: 1}));
        return path.split(".").reduce((obj, key) => obj[key], ss?.metrics);
    },
};

describe("change stream cursor metrics in serverStatus", function () {
    before(function () {
        testColl.drop();
        assert.commandWorked(testColl.insert({_id: 1}));
        this.openCursors = new CursorList();
    });

    beforeEach(function () {
        this.totalOpenedBefore = ServerStatusMetrics.getTotalOpened();
        this.lifespanBefore = ServerStatusMetrics.getLifespan();
    });

    afterEach(function () {
        // Close any cursors left open by a test (e.g. on failure).
        this.openCursors.closeAll();
    });

    after(function () {
        testColl.drop();
    });

    it("changeStreams.cursor.total_opened increases as change stream cursors are opened", function () {
        this.openCursors.add(testColl.watch(), testColl.watch(), testColl.watch());
        assert.eq(
            this.totalOpenedBefore + this.openCursors.length(),
            ServerStatusMetrics.getTotalOpened(),
            "totalOpened should increase by the number of opened change stream cursors",
        );
    });

    it("changeStreams.cursor.total_opened does not decrease when change stream cursors are closed", function () {
        this.openCursors.add(testColl.watch(), testColl.watch());
        const afterOpen = ServerStatusMetrics.getTotalOpened();

        this.openCursors.closeAll();

        // totalOpened is a counter — it must not decrease on close.
        assert.eq(afterOpen, ServerStatusMetrics.getTotalOpened());
    });

    it("changeStreams.cursor.lifespan histogram is populated after a change stream cursor is closed", function () {
        this.openCursors.add(testColl.watch());
        // The histogram should be unchanged while the cursor is still open.
        assert.docEq(this.lifespanBefore, ServerStatusMetrics.getLifespan());
        this.openCursors.closeAll();

        const lifespanHistogram = ServerStatusMetrics.getLifespan();
        assert.gte(lifespanHistogram.average, 0, "lifespan average must be non-negative");
        assert.eq(
            this.lifespanBefore.count + 1,
            lifespanHistogram.count,
            "lifespan count should increase by 1 after closing a change stream cursor",
        );
    });

    it("a regular (non-change-stream) cursor does not affect change stream metrics", function () {
        const regularCursorOpenBefore = ServerStatusMetrics.get("cursor.open.total");

        // Open a regular find cursor with batchSize:0. This returns an empty first batch, but
        // leaves the cursor open on the server, making it a live multi-batch cursor without it
        // being a change stream.
        const cursor = this.openCursors.add(
            new DBCommandCursor(
                testDB,
                assert.commandWorked(testDB.runCommand({find: testColl.getName(), filter: {}, batchSize: 0})),
                0,
            ),
        );
        assert.neq(cursor.getId(), 0, "unexpected empty cursorId");
        assert.eq(regularCursorOpenBefore + 1, ServerStatusMetrics.get("cursor.open.total"));

        assert.eq(this.totalOpenedBefore, ServerStatusMetrics.getTotalOpened());
        assert.docEq(this.lifespanBefore, ServerStatusMetrics.getLifespan());
    });
});
