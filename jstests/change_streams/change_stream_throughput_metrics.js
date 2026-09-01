/**
 * Tests that change stream throughput counters in serverStatus are incremented correctly
 * when documents, bytes, and batches are returned by change stream cursors.
 *
 * The docsReturned/bytesReturned/batchesReturned counters are reported by both the router (mongos)
 * and the data-bearing nodes (mongod). The docsExamined/bytesRead counters describe the oplog scan
 * and storage reads; on a data-bearing node they come from local execution, while on a mongos they
 * are aggregated from the shard responses in the AsyncResultsMerger. All counters advance
 * incrementally as each getMore batch is returned to the client.
 *
 * @tags: [
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
import {
    ServerStatusMetrics,
    TestDataModifyGuard,
} from "jstests/libs/query/change_stream_metrics_util.js";

describe("change stream throughput counters", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const testColl = testDB.getCollection("test");

    // Router-side counters, reported by whichever node (mongos or mongod) serves the change stream
    // cursor.
    const kRouterMetrics = ["docsReturned", "bytesReturned", "batchesReturned"];
    // Shard-side counters describing the oplog scan and storage reads. These are reported by
    // data-bearing nodes and, since SERVER-130705, also by a mongos (aggregated from the shard
    // responses in the AsyncResultsMerger and reported incrementally per getMore batch).
    const kShardMetrics = ["docsExamined", "bytesRead"];

    // Every throughput counter is now reported on both mongos and a replica set.
    const presentMetrics = kRouterMetrics.concat(kShardMetrics);

    function readThroughput() {
        const cursor = ServerStatusMetrics.getCsMetrics(db).cursor;
        const out = {};
        for (const metric of presentMetrics) {
            assert(cursor.hasOwnProperty(metric), `missing cursor.${metric}`, {cursor});
            out[metric] = cursor[metric];
        }
        return out;
    }

    before(function () {
        assertDropAndRecreateCollection(testDB, testColl.getName());
        // These are per-process counters, so all serverStatus reads and the change stream cursor
        // must go through the same router. Pin to a single mongos, otherwise before/after snapshots
        // may be served by different routers and the deltas become meaningless (or negative).
        this.testDataChange = new TestDataModifyGuard("pinToSingleMongos", true);
    });

    beforeEach(function () {
        this.cursorList = new CursorList();
    });

    afterEach(function () {
        this.cursorList.closeAll();
    });

    after(function () {
        assertDropCollection(testDB, testColl.getName());
        this.testDataChange.restore();
    });

    describe("present in serverStatus", function () {
        it("all throughput counters have numeric values", function () {
            const throughput = readThroughput();
            for (const metric of presentMetrics) {
                assert.gte(throughput[metric], 0, `missing cursor.${metric}`, {throughput});
            }
        });
    });

    // Counters that must strictly increase once a change stream has returned several getMore
    // batches: documents/bytes/batches returned and (on a data-bearing node) documents examined over
    // the oplog scan. We only check the aggregate before/after delta rather than each individual
    // batch, since a single getMore may examine several oplog entries while a later one returns a
    // buffered event without examining any. Exact per-call increment behavior is covered by the C++
    // unit tests (op_debug_test.cpp on the shard, cluster_client_cursor_impl_test.cpp on the
    // router).
    describe("increase as a change stream returns batches", function () {
        it("advances every throughput counter as it returns getMore batches", function () {
            const kNumBatches = 3;

            const before = readThroughput();

            // Open a change stream with batchSize:1 so each event is its own getMore batch.
            const csCursor = this.cursorList.push(testColl.watch([], {batchSize: 1}));
            for (let i = 0; i < kNumBatches; i++) {
                assert.commandWorked(testColl.insertOne({a: i}));
            }
            for (let i = 0; i < kNumBatches; i++) {
                assert.soon(() => csCursor.hasNext());
                csCursor.next();
            }

            // All counters advance incrementally per getMore batch, so they must have moved while
            // the cursor is still open. docsExamined strictly increases (the oplog scan examines the
            // inserted events); bytesRead can be served entirely from the WiredTiger cache, so it is
            // only non-decreasing.
            const strictlyIncreasing = kRouterMetrics.concat(["docsExamined"]);
            const nonDecreasing = ["bytesRead"];

            const after = readThroughput();
            for (const metric of strictlyIncreasing) {
                assert.gt(
                    after[metric],
                    before[metric],
                    `expected ${metric} to increase after the change stream returned batches`,
                    {metric, before, after},
                );
            }
            for (const metric of nonDecreasing) {
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
