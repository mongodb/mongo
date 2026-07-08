/**
 * Tests that change stream throughput counters in serverStatus are incremented correctly
 * when documents, bytes, and batches are returned by change stream cursors.
 *
 * TODO SERVER-129639: Remove assumes_against_mongod_not_mongos tag when MongoS metrics are added.
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_preference_unchanged,
 *   change_stream_does_not_expect_txns,
 *   uses_change_streams,
 *   requires_fcv_90,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {CursorList} from "jstests/libs/query/change_stream_util.js";
import {ServerStatusMetrics} from "jstests/change_streams/change_stream_metrics_util.js";

describe("change stream throughput counters", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const testColl = testDB.getCollection("test");

    before(function () {
        assertDropAndRecreateCollection(testDB, testColl.getName());
    });

    beforeEach(function () {
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        this.cursorList.closeAll();
    });

    after(function () {
        assertDropCollection(testDB, testColl.getName());
    });

    describe("present in serverStatus", function () {
        it("all throughput counters have numeric values", function () {
            const throughput = ServerStatusMetrics.getCsThroughputMetrics(db);

            assert.gte(throughput.docsReturned, 0, "missing cursor.docsReturned");
            assert.gte(throughput.bytesReturned, 0, "missing cursor.bytesReturned");
            assert.gte(throughput.batchesReturned, 0, "missing cursor.batchesReturned");
            assert.gte(throughput.docsExamined, 0, "missing cursor.docsExamined");
            assert.gte(throughput.bytesRead, 0, "missing cursor.bytesRead");
        });
    });

    // Counters that must strictly increase once a change stream has returned several getMore
    // batches: documents/bytes/batches returned and documents examined over the oplog scan. We only
    // check the aggregate before/after delta rather than each individual batch, since a single
    // getMore may examine several oplog entries while a later one returns a buffered event without
    // examining any. Exact per-call increment behavior is covered by the C++ unit test
    // (change_stream_throughput_metrics_test.cpp).
    const kStrictlyIncreasing = [
        "docsReturned",
        "bytesReturned",
        "batchesReturned",
        "docsExamined",
    ];
    // bytesRead reads from storage and can be served entirely from the WiredTiger cache, so it must
    // only be non-decreasing.
    const kNonDecreasing = ["bytesRead"];

    describe("increase as a change stream returns batches", function () {
        it("advances every throughput counter as it returns getMore batches", function () {
            const kNumBatches = 3;

            const before = ServerStatusMetrics.getCsThroughputMetrics(db);

            // Open a change stream with batchSize:1 so each event is its own getMore batch.
            const csCursor = this.cursorList.push(testColl.watch([], {batchSize: 1}));
            for (let i = 0; i < kNumBatches; i++) {
                assert.commandWorked(testColl.insertOne({a: i}));
            }
            for (let i = 0; i < kNumBatches; i++) {
                assert.soon(() => csCursor.hasNext());
                csCursor.next();
            }

            const after = ServerStatusMetrics.getCsThroughputMetrics(db);
            for (const metric of kStrictlyIncreasing) {
                assert.gt(
                    after[metric],
                    before[metric],
                    `expected ${metric} to increase after the change stream returned batches`,
                    {metric, before, after},
                );
            }
            for (const metric of kNonDecreasing) {
                assert.gte(
                    after[metric],
                    before[metric],
                    `expected ${metric} not to decrease after the change stream returned batches`,
                    {metric, before, after},
                );
            }
        });
    });
});
