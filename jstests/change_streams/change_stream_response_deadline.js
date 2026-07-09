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
 *   behavior reliable regardless of timer granularity.
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
 * The functionality is tested with both setting the query knob via setParameter and via query settings.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   # Cannot wrap initial insert operations into a single transaction, in order to make them
 *   # individual statements, not just a single applyOps command.
 *   change_stream_does_not_expect_txns,
 *   featureFlagAllowUserFacingQuerySettings,
 *   featureFlagPqsQueryKnobs,
 *   incompatible_aubsan,
 *   requires_fcv_90,
 *   requires_replication,
 *   tsan_incompatible,
 *   uses_change_streams,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";
import {ChangeStreamTest, getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {checkLog} from "src/mongo/shell/check_log.js";
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

    // Log ID emitted by PlanExecutorImpl::_handleResponseDeadline() at debug level 3 when the
    // operationResponseMaxMS deadline interrupts a collection scan.
    const kResponseDeadlineLogId = 10290000;

    const collName = jsTestName();

    let coll;
    let cst;
    let startTime;

    // Connections to replica set nodes  where log 10290000 is expected.
    // Populated in 'before()' after topology discovery.
    let replicaSetNodes = [];

    // Returns an array of Mongo connections to RS nodes. Works for both a direct RS connection (change_streams suite) and a mongos connection (sharded passthrough suites).
    const getReplicaSetNodes = (mongoConn) => {
        let nodes = [];
        const topology = DiscoverTopology.findConnectedNodes(mongoConn);
        if (topology.type === Topology.kReplicaSet) {
            topology.nodes.forEach((n) => {
                nodes.push({conn: new Mongo(n)});
            });
        } else if (topology.type === Topology.kShardedCluster) {
            Object.values(topology.shards)
                .filter((s) => s.type === Topology.kReplicaSet)
                .forEach((s) => {
                    s.nodes.forEach((n) => {
                        nodes.push({conn: new Mongo(n)});
                    });
                });
        } else {
            // Standalone topology is not supported for change streams, so this should never happen.
            throw new Error(`Unexpected topology type: ${topology.type}`);
        }
        return nodes;
    };

    const runWithFailPoint = (conn, failPointName, data = {}, cb) => {
        configureFailPointForAllShardsAndMongos({
            conn,
            failPointName,
            data,
            failPointMode: "alwaysOn",
        });
        try {
            return cb();
        } finally {
            configureFailPointForAllShardsAndMongos({conn, failPointName, failPointMode: "off"});
        }
    };

    // Test that a change stream opened on a collection returns multiple empty batches with a
    // postBatchResumeToken before it returns the single matching document.
    const runTest = (querySettings = undefined, withChangeStreamRestart = false) => {
        // Clear logs and capture near-zero offsets before the test runs. This avoids false
        // negatives caused by the ring buffer wrapping around when many entries accumulate.
        for (const node of replicaSetNodes) {
            node.conn.adminCommand({clearLog: "global"});
        }
        const logOffsets = replicaSetNodes.map((node) => checkLog.getGlobalLog(node.conn).length);

        runWithFailPoint(db.getMongo(), "hangCollScanDoWork", {delay: kDelayMS}, () => {
            // Open the change stream at startTime with batchSize: 0.
            let cursor = cst.startWatchingChanges({
                pipeline: [
                    {$changeStream: {startAtOperationTime: startTime}},
                    {$match: {"fullDocument._id": kNumDocs - 1}},
                ],
                collection: collName,
                aggregateOptions: {cursor: {batchSize: 0}},
                querySettings,
            });

            if (withChangeStreamRestart) {
                // Optionally restart change stream from the initial point.
                cursor = cst.restartChangeStream(cursor);
            }

            const initialPBRT = cursor.postBatchResumeToken;
            assert(initialPBRT, "Expected a postBatchResumeToken in the aggregate response", {
                cursor,
            });

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
                assert(pbrt, `Expected a postBatchResumeToken in getMore response ${i}`, {
                    cursor,
                });

                const pbrtCompareResult = bsonWoCompare(lastPBRT, pbrt);
                assert.lte(pbrtCompareResult, 0, `PBRT must be monotonically non-decreasing`);
                if (batch.length === 0) {
                    assert.neq(
                        NumberLong(0),
                        cursor.id,
                        "Cursor must stay open after an empty batch",
                        {cursor},
                    );
                } else {
                    assert.eq(
                        1,
                        batch.length,
                        "Expected exactly one document when the scan finds a match",
                        {
                            cursor,
                        },
                    );
                    foundDoc = batch[0];
                }
                if (pbrtCompareResult < 0) {
                    lastPBRT = pbrt;
                    differentPBRTsReturned++;
                }
            }

            assert(
                foundDoc !== null,
                "Did not receive the matching document within the iteration limit",
            );
            assert.eq(
                kNumDocs - 1,
                foundDoc.fullDocument._id,
                "Expected the last inserted document (_id: kNumDocs-1)",
                {foundDoc},
            );

            // Core assertion: multiple empty batches with postBatchResumeTokens were returned
            // before the matching document. The oplog scan will process createCollection and the
            // first (kNumDocs - 1) non-matching inserts before reaching the matching insert. Because
            // kDelayMS > kScanMaxMS, each doWork() call exhausts the scan budget and yields an empty
            // batch with an updated PBRT.
            assert.gte(
                differentPBRTsReturned,
                5,
                "Expected multiple empty batches with PBRT values before the matching document",
                {differentPBRTsReturned},
            );
        });

        // Verify that log 10290000 was emitted on at least one shard. In sharded passthrough mode
        // the collection is unsharded and only the primary shard will have a cursor, so "at least
        // one" is sufficient.
        assert.soon(
            () =>
                replicaSetNodes.some((node, i) => {
                    const offset = logOffsets[i];
                    return checkLog
                        .getGlobalLog(node.conn)
                        .slice(offset)
                        .some((log) => {
                            try {
                                return JSON.parse(log).id === kResponseDeadlineLogId;
                            } catch (e) {
                                return false;
                            }
                        });
                }),
            `Expected log ${kResponseDeadlineLogId} on at least one shard node, indicating ` +
                "the response deadline was applied and interrupted the collection scan",
        );
    };

    before(() => {
        coll = db.getCollection(collName);
        coll.drop();

        // Record the cluster time before creating the collection. The change stream will start
        // at T0, so the oplog scan covers the createCollection entry and all subsequent inserts.
        startTime = getClusterTime(db);

        assert.commandWorked(db.createCollection(collName));

        // Insert kNumDocs documents. The change stream $match passes only the last one
        // (_id: kNumDocs-1); every preceding entry is scanned and then discarded. Use individual inserts
        // to create multiple oplog entries instead of one large bundled applyOps entry.
        for (let i = 0; i < kNumDocs; ++i) {
            assert.commandWorked(coll.insert({_id: i}));
        }

        // Discover replica set nodes and enable verbosity 3 for the query log component so that
        // debug-level-3 log entries (including log 10290000) are visible. Dynamic setParameter is
        // used instead of startup parameters to avoid conflicts with resmoke's global verbosity.
        replicaSetNodes = getReplicaSetNodes(db.getMongo());
        for (const node of replicaSetNodes) {
            const res = assert.commandWorked(
                node.conn.adminCommand({
                    setParameter: 1,
                    logComponentVerbosity: {query: {verbosity: 3}},
                }),
            );
            node.previousQueryVerbosity = res.was.query;
        }
    });

    after(() => {
        assert(coll.drop());
        // Restore default query verbosity and close the connections opened for log checks.
        for (const node of replicaSetNodes) {
            assert.commandWorked(
                node.conn.adminCommand({
                    setParameter: 1,
                    logComponentVerbosity: {query: node.previousQueryVerbosity},
                }),
            );
            node.conn.close();
        }
        replicaSetNodes = [];
    });

    beforeEach(() => {
        cst = new ChangeStreamTest(db);
    });

    afterEach(() => {
        cst.cleanUp();
        cst = null;
    });

    [false, true].forEach((restartChangeStream) => {
        const testCaseSuffix = restartChangeStream ? " - with stream restart" : "";
        it(`returns multiple empty batches with a postBatchResumeToken - using server parameter${testCaseSuffix}`, () => {
            runWithParamsAllNonConfigNodes(db, {internalOperationResponseMaxMS: kScanMaxMS}, () => {
                runTest(undefined, restartChangeStream);
            });
        });

        it(`returns multiple empty batches with a postBatchResumeToken - using query settings override${testCaseSuffix}`, () => {
            runTest(
                {queryKnobs: {operationResponseMaxMS: NumberLong(kScanMaxMS)}},
                restartChangeStream,
            );
        });
    });
});
