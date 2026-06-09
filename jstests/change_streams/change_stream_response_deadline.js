/**
 * Tests that a change stream can bail out of a lengthy oplog scan periodically and return an
 * updated post-batch resume token (PBRT) on each interrupted getMore, via the
 * 'internalOperationResponseMaxMS' query knob.
 *
 * Why kDelayMS > kScanMaxMS:
 *   'hangCollScanDoWork' pauses each CollectionScan::doWork() call for kDelayMS milliseconds.
 *   The scan budget (kScanMaxMS) is checked at the TOP of the executor's inner loop, before
 *   each subsequent doWork() call. After the first doWork() completes and takes kDelayMS ms, the
 *   very next yield check fires and detects elapsed > kScanMaxMS. Using kDelayMS >
 *   kScanMaxMS guarantees that exactly ONE doWork() call exhausts the budget, making the
 *   behaviour reliable regardless of timer granularity.
 *
 * Test flow:
 *   1. Record clusterTime T0 before creating the collection.
 *   2. Create the collection and insert kNumDocs documents. With kDelayMS > kScanMaxMS, each
 *      oplog entry processed by a getMore causes the scan budget to expire on the very next yield
 *      check, returning an empty batch. The last inserted document (_id: kNumDocs-1) matches the
 *      change stream's $match filter.
 *   3. Open the change stream at T0 with batchSize:0 (initial postBatchResumeToken set).
 *   4. Issue getMore requests in a loop. Each non-matching oplog entry (createCollection or a
 *      non-matching insert) causes exactly one empty-batch getMore. Once the matching insert is
 *      reached, getMore returns the document.
 *   5. Assert that at least two empty batches were returned before the document, each carrying a
 *      postBatchResumeToken.
 *
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   assumes_stable_shard_list,
 *   assumes_unsharded_collection,
 *   # Cannot wrap initial insert operations into a single transaction, in order to make them
 *   # individual statements, not just a single applyOps command.
 *   change_stream_does_not_expect_txns,
 *   uses_change_streams,
 *   requires_fcv_90,
 *   requires_replication,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";
import {ChangeStreamTest, getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("change stream oplog scan PBRT updates via operationResponseMaxMS", () => {
    // kDelayMS is intentionally larger than kScanMaxMS. A single CollectionScan::doWork()
    // call sleeps kDelayMS, so on the very next yield check the callback detects
    // elapsed > kScanMaxMS and fires the deadline. This is reliable regardless of timer
    // granularity.
    const kScanMaxMS = 250;
    const kDelayMS = 300;

    // kNumDocs inserts generate kNumDocs + 1 oplog entries (createCollection + kNumDocs inserts).
    // Every non-matching entry causes one empty-batch getMore, so at least kNumDocs empty batches
    // are produced before the matching document is returned.
    const kNumDocs = 10;

    const collName = jsTestName();

    const runWithFailPoint = (conn, failPointName, data = {}, cb) => {
        configureFailPointForAllShardsAndMongos({conn, failPointName, data, failPointMode: "alwaysOn"});
        try {
            return cb();
        } finally {
            configureFailPointForAllShardsAndMongos({conn, failPointName, failPointMode: "off"});
        }
    };

    let coll;
    let cst;

    beforeEach(() => {
        coll = db.getCollection(collName);
        coll.drop();
        cst = new ChangeStreamTest(db);
    });

    afterEach(() => {
        cst.cleanUp();
        coll.drop();
    });

    it("returns multiple empty batches with a postBatchResumeToken before the single matching document", () => {
        // Record the cluster time before creating the collection. The change stream will start
        // at T0, so the oplog scan covers the createCollection entry and all subsequent inserts.
        const startTime = getClusterTime(db);

        assert.commandWorked(db.createCollection(collName));

        // Insert kNumDocs documents. The change stream $match passes only the last one
        // (_id: kNumDocs-1); every preceding entry is scanned and then discarded. Use individual inserts
        // to create multiple oplog entries instead of one large bundled applyOps entry.
        for (let i = 0; i < kNumDocs; ++i) {
            assert.commandWorked(coll.insert({_id: i}));
        }

        runWithParamsAllNonConfigNodes(db, {internalOperationResponseMaxMS: kScanMaxMS}, () => {
            runWithFailPoint(db.getMongo(), "hangCollScanDoWork", {delay: kDelayMS}, () => {
                // Open the change stream at startTime with batchSize:0.
                const cursor = cst.startWatchingChanges({
                    pipeline: [
                        {$changeStream: {startAtOperationTime: startTime}},
                        {$match: {"fullDocument._id": kNumDocs - 1}},
                    ],
                    collection: collName,
                    aggregateOptions: {cursor: {batchSize: 0}},
                });

                const initialPBRT = cursor.postBatchResumeToken;
                assert(initialPBRT, "Expected a postBatchResumeToken in the aggregate response", {cursor});

                let differentPBRTsReturned = 0;
                let foundDoc = null;
                let lastPBRT = initialPBRT;

                // Issue getMore requests until the matching document is returned. Each non-matching
                // oplog entry (createCollection, periodic no-ops, or non-matching inserts) causes the
                // scan budget to expire after one doWork() call, returning an empty batch. Once the
                // scan reaches the matching insert, the document is returned before the next yield check.
                for (let i = 0; i < 100 && foundDoc === null; i++) {
                    cst.getNextBatch(cursor);

                    const batch = cursor.nextBatch;
                    const pbrt = cursor.postBatchResumeToken;

                    // Every response must carry a postBatchResumeToken.
                    assert(pbrt, `Expected a postBatchResumeToken in getMore response ${i}`, {cursor});

                    const pbrtCompareResult = bsonWoCompare(lastPBRT, pbrt);
                    assert.lte(pbrtCompareResult, 0, `PBRT must be monotonically non-decreasing`);
                    if (batch.length === 0) {
                        assert.neq(NumberLong(0), cursor.id, "Cursor must stay open after an empty batch", {cursor});
                    } else {
                        assert.eq(1, batch.length, "Expected exactly one document when the scan finds a match", {
                            cursor,
                        });
                        foundDoc = batch[0];
                    }
                    if (pbrtCompareResult < 0) {
                        lastPBRT = pbrt;
                        differentPBRTsReturned++;
                    }
                }

                assert(foundDoc !== null, "Did not receive the matching document within the iteration limit");
                assert.eq(
                    kNumDocs - 1,
                    foundDoc.fullDocument._id,
                    "Expected the last inserted document (_id: kNumDocs-1)",
                    {foundDoc},
                );

                // Core assertion: multiple empty batches with postBatchResumeTokens were returned
                // before the matching document. With kNumDocs = 10, the oplog scan covers
                // createCollection + 2 non-matching inserts = at least 10 non-matching entries, each
                // causing one empty-batch getMore (because kDelayMS = 1500 ms > kScanMaxMS =
                // 200 ms, so a single doWork() call exhausts the scan budget).
                assert.gte(
                    differentPBRTsReturned,
                    5,
                    "Expected multiple empty batches with PBRT values before the matching document",
                    {differentPBRTsReturned},
                );
            });
        });
    });
});
