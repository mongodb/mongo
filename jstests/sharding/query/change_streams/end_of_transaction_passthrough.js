/**
 * Confirms that a shard-local change stream emits a user-visible endOfTransaction event for a
 * coordinated (multi-shard) transaction.
 *
 * A shard-local stream is a stream that runs entirely on one shard: either a
 * '$_passthroughToShard' stream (opened on mongos but forwarded to a single shard with
 * 'fromRouter: false') or a stream opened directly against a shard's mongod. Such a stream has no
 * router-side merge and cannot observe the transaction commits of the other participants, so
 * instead of delivering an accounting control event to a router it emits the user-visible
 * endOfTransaction event directly for every transaction committed on its shard. Opening one stream
 * per shard therefore yields one (duplicate) event per shard for the same transaction.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   featureFlagChangeStreamReaderV2,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {after, before, describe} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    itWhenEndOfTransactionIsUsable,
    withChangeStreamTest,
    withSession,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isTimestamp} from "jstests/libs/timestamp_util.js";

describe("endOfTransaction for a shard-local stream", function () {
    const dbA = jsTestName() + "_a"; // pinned to shard0
    const dbB = jsTestName() + "_b"; // pinned to shard1
    const collName = "coll";

    let st;
    let mongos;
    let shard0Name;
    let shard1Name;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 1}});
        st.stopBalancer();
        mongos = st.s;
        shard0Name = st.shard0.shardName;
        shard1Name = st.shard1.shardName;

        assert.commandWorked(mongos.adminCommand({enableSharding: dbA, primaryShard: shard0Name}));
        assert.commandWorked(mongos.adminCommand({enableSharding: dbB, primaryShard: shard1Name}));
        for (const dbName of [dbA, dbB]) {
            assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insertOne({_id: 0}));
        }
    });

    after(function () {
        st.stop();
    });

    // Runs a coordinated (multi-shard) transaction that writes one document with '_id: id' to each
    // shard, so it commits through two-phase commit.
    function runShardedTransaction(id) {
        withSession(mongos, (session) => {
            withTxnAndAutoRetryOnMongos(
                session,
                () => {
                    for (const dbName of [dbA, dbB]) {
                        assert.commandWorked(
                            session
                                .getDatabase(dbName)
                                .getCollection(collName)
                                .insertOne({_id: id}),
                        );
                    }
                },
                {readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}},
            );
        });
    }

    // Inserts a non-transactional document into the watched collection, to bound the stream after
    // the transaction's events.
    function insertSentinel(dbName, id) {
        assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insertOne({_id: id}));
    }

    // The expected change event of a non-transactional insert into the watched collection.
    function expectedInsert(dbName, id) {
        return {
            operationType: "insert",
            ns: {db: dbName, coll: collName},
            documentKey: {_id: id},
            fullDocument: {_id: id},
        };
    }

    // Starts a '$_passthroughToShard' change stream on 'shardId' watching the collection. The
    // stream is opened with 'showExpandedEvents' so the synthesized event carries its
    // 'operationDescription' field, and with 'showCommitTimestamp' so it carries its
    // 'commitTimestamp' field.
    function watchPassthrough(cst, shardId, showEndOfTransaction) {
        return cst.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        showEndOfTransaction,
                        showExpandedEvents: true,
                        showCommitTimestamp: true,
                    },
                },
            ],
            collection: collName,
            aggregateOptions: {$_passthroughToShard: {shard: shardId}},
        });
    }

    // Starts a change stream directly against the shard's mongod, watching the collection.
    function watchDirectToMongod(cst, showEndOfTransaction) {
        return cst.startWatchingChanges({
            pipeline: [
                {
                    $changeStream: {
                        showEndOfTransaction,
                        showExpandedEvents: true,
                        showCommitTimestamp: true,
                    },
                },
            ],
            collection: collName,
        });
    }

    // Asserts that 'event' is the user-visible endOfTransaction event of a cross-shard transaction.
    function assertEndOfTransactionEvent(event) {
        assert.eq(event.operationType, "endOfTransaction", "expected an endOfTransaction event", {
            event,
        });
        assert(isTimestamp(event.clusterTime), "missing clusterTime", {event});
        assert(
            isTimestamp(event.commitTimestamp),
            "a cross-shard endOfTransaction event must carry a commitTimestamp",
            {event},
        );
        assert(event.lsid, "missing lsid", {event});
        assert(event.txnNumber, "missing txnNumber", {event});
        assert(event.operationDescription, "missing operationDescription", {event});
        assert.eq(
            event.operationDescription.lsid,
            event.lsid,
            "operationDescription.lsid must match the top-level lsid",
            {event},
        );
        assert.eq(
            event.operationDescription.txnNumber,
            event.txnNumber,
            "operationDescription.txnNumber must match the top-level txnNumber",
            {event},
        );
        assert(
            !event.hasOwnProperty("ns") &&
                !event.hasOwnProperty("documentKey") &&
                !event.hasOwnProperty("fullDocument"),
            "an endOfTransaction event must not carry ns/documentKey/fullDocument",
            {event},
        );
    }

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a passthrough stream emits the event for a coordinated transaction",
        function () {
            runWithParamsAllNonConfigNodes(
                mongos.getDB("admin"),
                {allowEndOfTransaction: true},
                () => {
                    withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                        const cursor = watchPassthrough(cst, shard0Name, "whenAvailable");

                        runShardedTransaction(1);
                        insertSentinel(dbA, "sentinel-1");

                        // The transaction's insert into the watched collection is followed by its
                        // endOfTransaction event and then the sentinel, which bounds the stream.
                        const changes = cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                expectedInsert(dbA, 1),
                                {operationType: "endOfTransaction"},
                                expectedInsert(dbA, "sentinel-1"),
                            ],
                        });
                        assertEndOfTransactionEvent(changes[1]);
                    });
                },
            );
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a direct-to-mongod stream emits the event for a coordinated transaction",
        function () {
            runWithParamsAllNonConfigNodes(
                mongos.getDB("admin"),
                {allowEndOfTransaction: true},
                () => {
                    withChangeStreamTest(st.shard0.getDB(dbA), (cst) => {
                        const cursor = watchDirectToMongod(cst, "whenAvailable");

                        runShardedTransaction(2);
                        insertSentinel(dbA, "sentinel-2");

                        const changes = cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                expectedInsert(dbA, 2),
                                {operationType: "endOfTransaction"},
                                expectedInsert(dbA, "sentinel-2"),
                            ],
                        });
                        assertEndOfTransactionEvent(changes[1]);
                    });
                },
            );
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a passthrough stream on each shard emits its own event",
        function () {
            runWithParamsAllNonConfigNodes(
                mongos.getDB("admin"),
                {allowEndOfTransaction: true},
                () => {
                    // Each shard watches the collection it holds a copy of, so both are participants of
                    // the transaction's watched scope and each emits an endOfTransaction event.
                    withChangeStreamTest(mongos.getDB(dbA), (cst0) => {
                        withChangeStreamTest(mongos.getDB(dbB), (cst1) => {
                            const cursor0 = watchPassthrough(cst0, shard0Name, "whenAvailable");
                            const cursor1 = watchPassthrough(cst1, shard1Name, "whenAvailable");

                            runShardedTransaction(3);
                            insertSentinel(dbA, "sentinel-3a");
                            insertSentinel(dbB, "sentinel-3b");

                            const changes0 = cst0.assertNextChangesEqual({
                                cursor: cursor0,
                                expectedChanges: [
                                    expectedInsert(dbA, 3),
                                    {operationType: "endOfTransaction"},
                                    expectedInsert(dbA, "sentinel-3a"),
                                ],
                            });
                            assertEndOfTransactionEvent(changes0[1]);

                            const changes1 = cst1.assertNextChangesEqual({
                                cursor: cursor1,
                                expectedChanges: [
                                    expectedInsert(dbB, 3),
                                    {operationType: "endOfTransaction"},
                                    expectedInsert(dbB, "sentinel-3b"),
                                ],
                            });
                            assertEndOfTransactionEvent(changes1[1]);
                        });
                    });
                },
            );
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a passthrough required stream does not fail on an ineligible transaction",
        function () {
            runWithParamsAllNonConfigNodes(
                mongos.getDB("admin"),
                {allowEndOfTransaction: true},
                () => {
                    withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                        // A 'required' shard-local stream treats 'required' as 'whenAvailable'.
                        const cursor = watchPassthrough(cst, shard0Name, "required");

                        // Commit the transaction while the write path is disabled, so its commit entry
                        // carries no accounting metadata and is ineligible.
                        runWithParamsAllNonConfigNodes(
                            mongos.getDB("admin"),
                            {allowEndOfTransaction: false},
                            () => {
                                runShardedTransaction(4);
                            },
                        );
                        insertSentinel(dbA, "sentinel-4");

                        // The stream does not fail; it emits only the transaction's insert and the
                        // sentinel.
                        cst.assertNextChangesEqual({
                            cursor,
                            expectedChanges: [
                                expectedInsert(dbA, 4),
                                expectedInsert(dbA, "sentinel-4"),
                            ],
                        });
                    });
                },
            );
        },
    );
});
