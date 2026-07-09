/**
 * Tests that the change stream cursor option histogram metrics
 * (metrics.changeStreams.option.cursor.batchSize and metrics.changeStreams.option.cursor.maxTimeMS)
 * are NOT recorded for ordinary (non-change-stream) aggregate/getMore commands, even when those
 * commands set the same request fields. Verified on both mongod and mongos.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ServerStatusMetrics} from "jstests/change_streams/change_stream_metrics_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function openAndCloseChangeStream(db, collName) {
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: collName,
            pipeline: [{$changeStream: {}}],
            cursor: {batchSize: 0},
            maxTimeMS: 0,
        }),
    );
    db.runCommand({killCursors: collName, cursors: [res.cursor.id]});
}

function runPlainAggregate(db, collName, {batchSize, maxTimeMS} = {}) {
    const cmd = {
        aggregate: collName,
        pipeline: [{$match: {}}],
        cursor: batchSize === undefined ? {} : {batchSize},
    };
    if (maxTimeMS !== undefined) {
        cmd.maxTimeMS = maxTimeMS;
    }
    return assert.commandWorked(db.runCommand(cmd));
}

function runGetMore(db, collName, cursorId, {batchSize} = {}) {
    const cmd = {getMore: cursorId, collection: collName};
    if (batchSize !== undefined) {
        cmd.batchSize = batchSize;
    }
    return assert.commandWorked(db.runCommand(cmd));
}

function killCursor(db, collName, cursorId) {
    db.runCommand({killCursors: collName, cursors: [cursorId]});
}

function buildTests() {
    it("aggregate cursor.batchSize is not recorded for a non-change-stream aggregate", function () {
        const before = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);

        // batchSize:1 keeps the cursor open (rather than exhausting it inline) so the command
        // still goes through the same cursor-establishing code path as a change stream would.
        const res = runPlainAggregate(this.testDB, this.collName, {batchSize: 1});
        try {
            const after = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            assert.eq(
                after.totalCount,
                before.totalCount,
                "batchSize should not be recorded for a non-change-stream aggregate",
                {before, after},
            );
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("aggregate maxTimeMS is not recorded for a non-change-stream aggregate", function () {
        const before = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

        const res = runPlainAggregate(this.testDB, this.collName, {
            batchSize: 1,
            maxTimeMS: 1500,
        });
        try {
            const after = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
            assert.eq(
                after.totalCount,
                before.totalCount,
                "maxTimeMS should not be recorded for a non-change-stream aggregate",
                {before, after},
            );
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("getMore cursor.batchSize is not recorded for a non-change-stream cursor", function () {
        // maxTimeMS on getMore is illegal for a non-awaitData cursor, so only batchSize is
        // exercised here; the maxTimeMS-on-aggregate case above already covers that field.
        const res = runPlainAggregate(this.testDB, this.collName, {batchSize: 1});
        try {
            const before = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);

            runGetMore(this.testDB, this.collName, res.cursor.id, {batchSize: 50});

            const after = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            assert.eq(
                after.totalCount,
                before.totalCount,
                "batchSize should not be recorded for a non-change-stream getMore",
                {before, after},
            );
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });
}

describe("change stream cursor option metrics do not track non-change-stream commands on mongod", function () {
    before(function () {
        this.rst = new ReplSetTest({nodes: 1});
        this.rst.startSet();
        this.rst.initiate();
        this.testDB = this.rst.getPrimary().getDB(jsTestName());
        this.collName = "mongod_test";
        assertDropAndRecreateCollection(this.testDB, this.collName);
        assert.commandWorked(this.testDB.getCollection(this.collName).insertMany([{a: 1}, {a: 2}]));

        // Warm up the histograms so this test can safely read the 'option.cursor' keys regardless of
        // whether they have recorded any data points yet.
        openAndCloseChangeStream(this.testDB, this.collName);
    });

    after(function () {
        assertDropCollection(this.testDB, this.collName);
        this.rst.stopSet();
    });

    buildTests();
});

describe("change stream cursor option metrics do not track non-change-stream commands on mongos", function () {
    before(function () {
        this.st = new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}});
        this.testDB = this.st.s.getDB(jsTestName());
        this.collName = "mongos_test";
        assertDropAndRecreateCollection(this.testDB, this.collName);
        assert.commandWorked(this.testDB.getCollection(this.collName).insertMany([{a: 1}, {a: 2}]));

        // Warm up the histograms: they are lazily registered with serverStatus on the first call
        // to recordCursorOptionMetrics, so a fresh server has no 'option.cursor' key at all.
        openAndCloseChangeStream(this.testDB, this.collName);
    });

    after(function () {
        assertDropCollection(this.testDB, this.collName);
        this.st.stop();
    });

    buildTests();
});
