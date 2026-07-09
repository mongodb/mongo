/**
 * Tests that serverStatus correctly reflects the change stream cursor option histogram metrics
 * (metrics.changeStreams.option.cursor.batchSize and metrics.changeStreams.option.cursor.maxTimeMS)
 * when change stream aggregate/getMore commands set these options. The metrics are registered
 * with ClusterRole::None, so they must be exercised on both mongod and mongos.
 *
 * Coverage includes:
 * - boundary values that land in every histogram bucket, including the lowest (underflow) and
 *   highest (overflow) buckets, for both the batchSize and maxTimeMS histograms;
 * - aggregate and getMore commands that omit maxTimeMS, which must not increment the maxTimeMS
 *   histogram at all;
 * - getMore commands that omit batchSize, which must not increment the batchSize histogram at
 *   all;
 * - aggregate commands that omit cursor.batchSize: unlike getMore, the aggregate command's IDL
 *   parser (parseAggregateCursorFromBSON) backfills the default batchSize (101) into the parsed
 *   request whenever the client doesn't specify one, so the batchSize histogram is *always*
 *   incremented for aggregate, landing in the bucket for the default value when unset.
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

// Boundary values for the batchSize histogram (bucket boundaries: 1, 10, 100, 1000, 10000).
// Each value is the lower bound of the bucket it is expected to land in, since histogram
// buckets are lower-inclusive ("[lo, hi)").
const kBatchSizeBucketCases = [
    {value: 0, bucket: "(-inf, 1)"},
    {value: 1, bucket: "[1, 10)"},
    {value: 10, bucket: "[10, 100)"},
    {value: 100, bucket: "[100, 1000)"},
    {value: 1000, bucket: "[1000, 10000)"},
    {value: 10000, bucket: "[10000, inf)"},
];

// Boundary values for the maxTimeMS histogram (bucket boundaries: 100, 200, 500, 1000, 2000,
// 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000).
const kMaxTimeMSBucketCases = [
    {value: 0, bucket: "(-inf, 100)"},
    {value: 100, bucket: "[100, 200)"},
    {value: 200, bucket: "[200, 500)"},
    {value: 500, bucket: "[500, 1000)"},
    {value: 1000, bucket: "[1000, 2000)"},
    {value: 2000, bucket: "[2000, 5000)"},
    {value: 5000, bucket: "[5000, 10000)"},
    {value: 10000, bucket: "[10000, 20000)"},
    {value: 20000, bucket: "[20000, 50000)"},
    {value: 50000, bucket: "[50000, 100000)"},
    {value: 100000, bucket: "[100000, 200000)"},
    {value: 200000, bucket: "[200000, 500000)"},
    {value: 500000, bucket: "[500000, 1000000)"},
    {value: 1000000, bucket: "[1000000, inf)"},
];

// A small set of representative batchSize/maxTimeMS combinations, covering the lowest, a
// mid-range, and the highest bucket of each histogram, exercised via getMore.
const kGetMoreOptionCases = [
    {batchSize: 0, batchSizeBucket: "(-inf, 1)", maxTimeMS: 0, maxTimeMSBucket: "(-inf, 100)"},
    {
        batchSize: 500,
        batchSizeBucket: "[100, 1000)",
        maxTimeMS: 20000,
        maxTimeMSBucket: "[20000, 50000)",
    },
    {
        batchSize: 10000,
        batchSizeBucket: "[10000, inf)",
        maxTimeMS: 1000000,
        maxTimeMSBucket: "[1000000, inf)",
    },
];

function openChangeStream(db, collName, {batchSize, maxTimeMS} = {}) {
    const cmd = {
        aggregate: collName,
        pipeline: [{$changeStream: {}}],
        cursor: batchSize === undefined ? {} : {batchSize},
    };
    if (maxTimeMS !== undefined) {
        cmd.maxTimeMS = maxTimeMS;
    }
    return assert.commandWorked(db.runCommand(cmd));
}

function runGetMore(db, collName, cursorId, {batchSize, maxTimeMS} = {}) {
    const cmd = {getMore: cursorId, collection: collName};
    if (batchSize !== undefined) {
        cmd.batchSize = batchSize;
    }
    if (maxTimeMS !== undefined) {
        cmd.maxTimeMS = maxTimeMS;
    }
    return assert.commandWorked(db.runCommand(cmd));
}

function killCursor(db, collName, cursorId) {
    db.runCommand({killCursors: collName, cursors: [cursorId]});
}

// Asserts that recording 'label' incremented the histogram's totalCount by 1, and that the
// increment landed in 'bucket'.
function assertBucketIncrement(before, after, bucket, label) {
    assert.eq(
        after.totalCount,
        before.totalCount + 1,
        `expected ${label} totalCount to increase by 1`,
        {before, after},
    );
    assert.eq(
        after[bucket].count,
        before[bucket].count + 1,
        `expected ${label} to land in the ${bucket} bucket`,
        {before, after},
    );
}

// Asserts that no command lacking 'label' incremented the histogram's totalCount.
function assertNoIncrement(before, after, label) {
    assert.eq(
        after.totalCount,
        before.totalCount,
        `expected ${label} totalCount to stay unchanged`,
        {
            before,
            after,
        },
    );
}

function buildTests() {
    it("aggregate cursor.batchSize is recorded for a change stream", function () {
        const before = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);

        const res = openChangeStream(this.testDB, this.collName, {batchSize: 5});
        try {
            const after = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            assertBucketIncrement(before, after, "[1, 10)", "batchSize=5");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("aggregate maxTimeMS is recorded for a change stream", function () {
        const before = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

        const res = openChangeStream(this.testDB, this.collName, {maxTimeMS: 1500});
        try {
            const after = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
            assertBucketIncrement(before, after, "[1000, 2000)", "maxTimeMS=1500");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("getMore cursor option metrics are recorded for a change stream", function () {
        const res = openChangeStream(this.testDB, this.collName);
        try {
            // Insert a document so the getMore below has an event ready and returns immediately,
            // rather than blocking until maxTimeMS elapses.
            assert.commandWorked(this.testDB.getCollection(this.collName).insertOne({a: 1}));

            const beforeBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            const beforeMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

            runGetMore(this.testDB, this.collName, res.cursor.id, {
                batchSize: 50,
                maxTimeMS: 300,
            });

            const afterBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            const afterMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

            assertBucketIncrement(beforeBatchSize, afterBatchSize, "[10, 100)", "batchSize=50");
            assertBucketIncrement(beforeMaxTimeMS, afterMaxTimeMS, "[200, 500)", "maxTimeMS=300");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("aggregate cursor.batchSize lands in the correct bucket across the full histogram range", function () {
        for (const {value, bucket} of kBatchSizeBucketCases) {
            const before = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);

            const res = openChangeStream(this.testDB, this.collName, {batchSize: value});
            try {
                const after = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
                assertBucketIncrement(before, after, bucket, `batchSize=${value}`);
            } finally {
                killCursor(this.testDB, this.collName, res.cursor.id);
            }
        }
    });

    it("aggregate maxTimeMS lands in the correct bucket across the full histogram range", function () {
        for (const {value, bucket} of kMaxTimeMSBucketCases) {
            const before = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

            const res = openChangeStream(this.testDB, this.collName, {maxTimeMS: value});
            try {
                const after = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
                assertBucketIncrement(before, after, bucket, `maxTimeMS=${value}`);
            } finally {
                killCursor(this.testDB, this.collName, res.cursor.id);
            }
        }
    });

    it("getMore cursor option metrics land in the correct bucket across the histogram range", function () {
        const res = openChangeStream(this.testDB, this.collName);
        try {
            for (const {
                batchSize,
                batchSizeBucket,
                maxTimeMS,
                maxTimeMSBucket,
            } of kGetMoreOptionCases) {
                // Insert a document so the getMore below has an event ready and returns
                // immediately, rather than blocking until maxTimeMS elapses.
                assert.commandWorked(this.testDB.getCollection(this.collName).insertOne({a: 1}));

                const beforeBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
                const beforeMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

                runGetMore(this.testDB, this.collName, res.cursor.id, {batchSize, maxTimeMS});

                const afterBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
                const afterMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

                assertBucketIncrement(
                    beforeBatchSize,
                    afterBatchSize,
                    batchSizeBucket,
                    `batchSize=${batchSize}`,
                );
                assertBucketIncrement(
                    beforeMaxTimeMS,
                    afterMaxTimeMS,
                    maxTimeMSBucket,
                    `maxTimeMS=${maxTimeMS}`,
                );
            }
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("aggregate without cursor.batchSize records the backfilled default batchSize", function () {
        // Unlike getMore, the aggregate command's IDL parser backfills the default batchSize
        // (101) into the request whenever the client omits cursor.batchSize, so the histogram is
        // still incremented, landing in the bucket for the default value.
        const before = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);

        const res = openChangeStream(this.testDB, this.collName, {maxTimeMS: 1000});
        try {
            const after = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            assertBucketIncrement(before, after, "[100, 1000)", "the default batchSize (101)");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("aggregate without maxTimeMS does not increment the maxTimeMS histogram", function () {
        const before = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

        const res = openChangeStream(this.testDB, this.collName, {batchSize: 5});
        try {
            const after = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
            assertNoIncrement(before, after, "maxTimeMS");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("aggregate without cursor.batchSize or maxTimeMS records the default batchSize only", function () {
        const beforeBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
        const beforeMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

        const res = openChangeStream(this.testDB, this.collName);
        try {
            const afterBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            const afterMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
            assertBucketIncrement(
                beforeBatchSize,
                afterBatchSize,
                "[100, 1000)",
                "the default batchSize (101)",
            );
            assertNoIncrement(beforeMaxTimeMS, afterMaxTimeMS, "maxTimeMS");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("getMore without batchSize does not increment the batchSize histogram", function () {
        const res = openChangeStream(this.testDB, this.collName);
        try {
            assert.commandWorked(this.testDB.getCollection(this.collName).insertOne({a: 1}));

            const before = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);

            runGetMore(this.testDB, this.collName, res.cursor.id, {maxTimeMS: 1000});

            const after = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            assertNoIncrement(before, after, "batchSize");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("getMore without maxTimeMS does not increment the maxTimeMS histogram", function () {
        const res = openChangeStream(this.testDB, this.collName);
        try {
            assert.commandWorked(this.testDB.getCollection(this.collName).insertOne({a: 1}));

            const before = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

            runGetMore(this.testDB, this.collName, res.cursor.id, {batchSize: 5});

            const after = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
            assertNoIncrement(before, after, "maxTimeMS");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });

    it("getMore without batchSize or maxTimeMS does not increment either histogram", function () {
        const res = openChangeStream(this.testDB, this.collName);
        try {
            assert.commandWorked(this.testDB.getCollection(this.collName).insertOne({a: 1}));

            const beforeBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            const beforeMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);

            runGetMore(this.testDB, this.collName, res.cursor.id);

            const afterBatchSize = ServerStatusMetrics.getCsCursorOptionBatchSize(this.testDB);
            const afterMaxTimeMS = ServerStatusMetrics.getCsCursorOptionMaxTimeMS(this.testDB);
            assertNoIncrement(beforeBatchSize, afterBatchSize, "batchSize");
            assertNoIncrement(beforeMaxTimeMS, afterMaxTimeMS, "maxTimeMS");
        } finally {
            killCursor(this.testDB, this.collName, res.cursor.id);
        }
    });
}

describe("change stream cursor option metrics on mongod", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        this.rst.startSet();
        this.rst.initiate();
        this.testDB = this.rst.getPrimary().getDB(jsTestName());
        this.collName = "mongod_test";
        assertDropAndRecreateCollection(this.testDB, this.collName);
    });

    after(function () {
        assertDropCollection(this.testDB, this.collName);
        this.rst.stopSet();
    });

    buildTests();
});

describe("change stream cursor option metrics on mongos", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        this.testDB = this.st.s.getDB(jsTestName());
        this.collName = "mongos_test";
        assertDropAndRecreateCollection(this.testDB, this.collName);
    });

    after(function () {
        assertDropCollection(this.testDB, this.collName);
        this.st.stop();
    });

    buildTests();
});
