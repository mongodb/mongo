/**
 * Confirms that a sharded (coordinated multi-shard) transaction produces an endOfTransaction change
 * event on a router (mongos) change stream, end-to-end.
 *
 * A coordinated transaction commits through two-phase commit: each write-participant shard writes a
 * 'commitTransaction' oplog entry carrying the 'endOfTransactionScopes' accounting metadata. The
 * shard's unwind stage turns that entry into an internal accounting control event, which travels to
 * the router unchanged. The router's topology-change stage passes it through and the router's
 * synthesis stage emits the user-visible 'endOfTransaction' event once the transaction is complete
 * for the watched scope.
 *
 * The feature flag is enabled through the tag below; the 'allowEndOfTransaction' server parameter is
 * enabled per case. Each database is pinned to a different shard, so a transaction that writes to
 * both databases uses two-phase commit.
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
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    assertGetMoreFailsWithExpectedError,
    itWhenEndOfTransactionIsUsable,
    withChangeStreamTest,
    withSession,
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isTimestamp} from "jstests/libs/timestamp_util.js";

describe("endOfTransaction for a sharded transaction", function () {
    const dbA = jsTestName() + "_a"; // pinned to shard0
    const dbB = jsTestName() + "_b"; // pinned to shard1
    const collName = "coll";
    // A collection sharded so that it has one chunk on each shard. A transaction that writes it on
    // both shards makes both shards participants of the watched scope.
    const sharedCollName = "sharedColl";

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

        // Create the collections outside the transactions so that they contain exactly the writes
        // performed below.
        for (const dbName of [dbA, dbB]) {
            assert.commandWorked(mongos.getDB(dbName).getCollection(collName).insertOne({_id: 0}));
        }

        // Shard 'sharedColl' with one chunk on each shard: '_id: 1' is routed to shard1 and
        // '_id: 100' to shard0.
        assert.commandWorked(
            mongos.adminCommand({shardCollection: dbA + "." + sharedCollName, key: {_id: 1}}),
        );
        assert.commandWorked(
            mongos.adminCommand({split: dbA + "." + sharedCollName, middle: {_id: 100}}),
        );
        assert.commandWorked(
            mongos.adminCommand({
                moveChunk: dbA + "." + sharedCollName,
                find: {_id: 0},
                to: shard1Name,
            }),
        );
    });

    after(function () {
        st.stop();
    });

    // Commits a coordinated (multi-shard) transaction that writes one document with '_id: id' to
    // each of the two shards, so it commits through two-phase commit. The 'allowEndOfTransaction'
    // server parameter is left untouched, so the caller decides whether the commit is eligible.
    function commitCrossShardTransaction(id) {
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

    // Runs the cross-shard transaction with the end-of-transaction write path enabled.
    function runShardedTransaction(id) {
        runWithParamsAllNonConfigNodes(mongos.getDB("admin"), {allowEndOfTransaction: true}, () => {
            commitCrossShardTransaction(id);
        });
    }

    // Inserts a non-transactional document into the watched collection, to bound the stream after
    // the transaction's events.
    function insertSentinel(id) {
        assert.commandWorked(mongos.getDB(dbA).getCollection(collName).insertOne({_id: id}));
    }

    // The expected change event of a non-transactional insert into a watched collection.
    function expectedInsert(dbName, id, collection = collName) {
        return {
            operationType: "insert",
            ns: {db: dbName, coll: collection},
            documentKey: {_id: id},
            fullDocument: {_id: id},
        };
    }

    // Returns the timestamp of the transaction's 'commitTransaction' oplog entry on 'shard'.
    function getCommitEntryTs(shard, lsid, txnNumber) {
        const entries = shard
            .getDB("local")
            .oplog.rs.find({"lsid.id": lsid.id, txnNumber, "o.commitTransaction": 1})
            .toArray();
        assert.eq(1, entries.length, "expected exactly one commitTransaction entry", {
            shardName: shard.shardName,
            entries,
        });
        return entries[0].ts;
    }

    // Waits until 'shard' has written the transaction's 'commitTransaction' oplog entry and returns
    // its timestamp.
    function waitForCommitEntryTs(shard, lsid, txnNumber) {
        let ts;
        assert.soon(
            () => {
                const entries = shard
                    .getDB("local")
                    .oplog.rs.find({"lsid.id": lsid.id, txnNumber, "o.commitTransaction": 1})
                    .toArray();
                if (entries.length === 0) {
                    return false;
                }
                assert.eq(1, entries.length, "expected exactly one commitTransaction entry", {
                    shardName: shard.shardName,
                    entries,
                });
                ts = entries[0].ts;
                return true;
            },
            "timed out waiting for the commitTransaction entry",
            30 * 1000,
        );
        return ts;
    }

    // Advances the cluster time of 'conn's shard to the given 'secs' by gossiping a future cluster
    // time. A burst of ordinary writes cannot be used for this: such writes only advance the
    // increment within the current wall-clock second, and that increment is discarded as soon as the
    // wall clock rolls over to the next second (see VectorClockMutable). Gossiping a timestamp whose
    // seconds component is already in the future makes any subsequent write (such as a prepared
    // transaction commit) receive a strictly later oplog timestamp deterministically.
    function advanceShardClusterTime(conn, secs) {
        const {$clusterTime} = assert.commandWorked(conn.adminCommand({hello: 1}));
        conn.advanceClusterTime({
            clusterTime: Timestamp(secs, 0),
            signature: $clusterTime.signature,
        });
        assert.commandWorked(conn.adminCommand({ping: 1}));
    }

    // Opens a collection-scoped change stream on dbA.coll (shard0) with the given
    // 'showEndOfTransaction' mode, and returns the cursor. The stream is opened with
    // 'showExpandedEvents' so the synthesized event carries its 'operationDescription' field, and
    // with 'showCommitTimestamp' so it carries its 'commitTimestamp' field.
    function openStream(
        cst,
        showEndOfTransaction,
        extraStages,
        {startAtOperationTime, collection = collName} = {},
    ) {
        const csOptions = {
            showEndOfTransaction,
            showExpandedEvents: true,
            showCommitTimestamp: true,
        };
        if (startAtOperationTime) {
            csOptions.startAtOperationTime = startAtOperationTime;
        }
        const pipeline = [{$changeStream: csOptions}].concat(extraStages || []);
        return cst.startWatchingChanges({pipeline, collection});
    }

    // Asserts that 'event' is the user-visible endOfTransaction event of the transaction whose
    // change event is 'txnEvent'.
    function assertEndOfTransactionEvent(event, txnEvent) {
        assert.eq(event.operationType, "endOfTransaction", "expected an endOfTransaction event", {
            event,
        });
        assert(isTimestamp(event.clusterTime), "missing clusterTime", {event});
        assert(
            isTimestamp(event.commitTimestamp),
            "a cross-shard endOfTransaction event must carry a commitTimestamp",
            {event},
        );
        assert.eq(event.lsid, txnEvent.lsid, "unexpected lsid", {event, txnEvent});
        assert.eq(event.txnNumber, txnEvent.txnNumber, "unexpected txnNumber", {event, txnEvent});
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
        "emits an endOfTransaction event for a sharded transaction",
        function () {
            withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                const cursor = openStream(cst, "whenAvailable");

                runShardedTransaction(1);
                insertSentinel("sentinel-1");

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
                assertEndOfTransactionEvent(changes[1], changes[0]);
            });
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a required stream emits the event for a sharded transaction",
        function () {
            withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                // A 'required' stream must not fail on a coordinated transaction: its event is produced
                // from the accounting metadata by the router.
                const cursor = openStream(cst, "required");

                runShardedTransaction(2);
                insertSentinel("sentinel-2");

                const changes = cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        expectedInsert(dbA, 2),
                        {operationType: "endOfTransaction"},
                        expectedInsert(dbA, "sentinel-2"),
                    ],
                });
                assertEndOfTransactionEvent(changes[1], changes[0]);
            });
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a required router stream fails on an ineligible transaction",
        function () {
            runWithParamsAllNonConfigNodes(
                mongos.getDB("admin"),
                {allowEndOfTransaction: true},
                () => {
                    withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                        // A 'required' stream opened through the router has a router-side merge and must
                        // fail when it reads an ineligible transaction at its watched scope.
                        const cursor = openStream(cst, "required");

                        // Commit the transaction while the write path is disabled, so its commit entries
                        // carry neither the 'allowEndOfTransaction' marker nor the accounting metadata and
                        // are ineligible.
                        runWithParamsAllNonConfigNodes(
                            mongos.getDB("admin"),
                            {allowEndOfTransaction: false},
                            () => {
                                commitCrossShardTransaction(4);
                            },
                        );

                        // The stream must fail because the required endOfTransaction event cannot be
                        // produced. The transaction commit is asynchronous, so poll until the stream
                        // reaches it and raises the error.
                        const error = assertGetMoreFailsWithExpectedError({
                            db: mongos.getDB(dbA),
                            cursorId: cursor.id,
                            collName,
                            expectedError: (res) => res.code === ErrorCodes.ChangeStreamFatalError,
                            msg: "timed out waiting for the stream to fail on the ineligible transaction",
                        });
                        assert(
                            error.errmsg.includes("no endOfTransaction event could be generated"),
                            "unexpected error message for the unproducible endOfTransaction event",
                            {error},
                        );
                    });
                },
            );
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a user $match filters out the synthesized event",
        function () {
            withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                // The user's $match admits only inserts, so the synthesized endOfTransaction event is
                // filtered out and never emitted.
                const cursor = openStream(cst, "whenAvailable", [
                    {$match: {operationType: "insert"}},
                ]);

                runShardedTransaction(3);
                insertSentinel("sentinel-3");

                cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [expectedInsert(dbA, 3), expectedInsert(dbA, "sentinel-3")],
                });
            });
        },
    );

    itWhenEndOfTransactionIsUsable(
        () => mongos.getDB(dbA),
        "a clusterTime $match preserves a participant's accounting commit",
        function () {
            // Both shards write the watched collection, so both are participants of the watched scope.
            // The first write targets the chunk on shard0, making shard0 the transaction coordinator.
            const txnNumber = NumberLong(1);
            let lsid;
            let awaitCommit;
            let tsShard0;
            withSession(mongos, (session) => {
                lsid = session.getSessionId();
                const sessionDb = session.getDatabase(dbA);
                try {
                    runWithParamsAllNonConfigNodes(
                        mongos.getDB("admin"),
                        {allowEndOfTransaction: true},
                        () => {
                            assert.commandWorked(
                                sessionDb.runCommand({
                                    insert: sharedCollName,
                                    documents: [{_id: 100}], // chunk on shard0 -> coordinator
                                    lsid,
                                    txnNumber,
                                    startTransaction: true,
                                    autocommit: false,
                                    readConcern: {level: "snapshot"},
                                }),
                            );
                            assert.commandWorked(
                                sessionDb.runCommand({
                                    insert: sharedCollName,
                                    documents: [{_id: 1}], // chunk on shard1
                                    lsid,
                                    txnNumber,
                                    autocommit: false,
                                }),
                            );

                            // Pause the coordinator's worker that sends the commit to the remote
                            // participant (shard1). The local participant (shard0) still commits, so once
                            // shard0's commit entry exists, advance shard1's cluster time past shard0's
                            // commit timestamp and let the commit proceed. shard1 then commits at a
                            // strictly later oplog timestamp than shard0. Waiting for shard0 to commit
                            // first matters: the coordinator gossips its own cluster time with the
                            // commit command, so if shard0 had not committed yet it could pick up
                            // shard1's advanced time and both entries would end up equal.
                            withFailPoint(
                                st.shard0,
                                "hangWhileTargetingRemoteHost",
                                (fp) => {
                                    awaitCommit = startParallelShell(
                                        `assert.commandWorked(db.getSiblingDB('admin').runCommand({` +
                                            `commitTransaction: 1, lsid: ${tojson(lsid)}, ` +
                                            `txnNumber: NumberLong(${txnNumber}), autocommit: false}));`,
                                        mongos.port,
                                    );
                                    fp.wait();
                                    tsShard0 = waitForCommitEntryTs(st.shard0, lsid, txnNumber);
                                    advanceShardClusterTime(st.shard1, tsShard0.getTime() + 100);
                                },
                                {data: {twoPhaseCommitStage: "decision"}},
                            );
                            awaitCommit();
                            awaitCommit = undefined;
                        },
                    );
                } finally {
                    if (awaitCommit) {
                        awaitCommit();
                    }
                }
            });

            // The two participants committed at different oplog timestamps. Take the later one as the
            // clusterTime threshold so that the predicate excludes the earlier participant's commit
            // entry from the oplog match. The synthesized event carries the later timestamp, so it must
            // still be emitted: the earlier participant's accounting commit must be preserved
            // independently of the user's clusterTime predicate, which is applied to the synthesized
            // event on the router instead.
            const tsShard1 = getCommitEntryTs(st.shard1, lsid, txnNumber);
            const cmp = timestampCmp(tsShard0, tsShard1);
            assert.neq(cmp, 0, "expected the two commit entries to carry different timestamps", {
                tsShard0,
                tsShard1,
            });
            const earlier = cmp < 0 ? tsShard0 : tsShard1;
            const later = cmp < 0 ? tsShard1 : tsShard0;

            // Bound the stream after the synthesized event.
            assert.commandWorked(
                mongos.getDB(dbA).getCollection(sharedCollName).insertOne({_id: 200}),
            );

            withChangeStreamTest(mongos.getDB(dbA), (cst) => {
                // Start the stream before the transaction and admit only events whose clusterTime is at
                // least 'later'.
                const cursor = openStream(
                    cst,
                    "whenAvailable",
                    [{$match: {clusterTime: {$gte: later}}}],
                    {startAtOperationTime: earlier, collection: sharedCollName},
                );

                // The insert on the shard that committed later and the synthesized endOfTransaction
                // event both carry 'later' as their clusterTime. Without preserving the earlier
                // participant's accounting commit, the transaction never completes for the watched
                // scope and the synthesized event is missing (the second event would be the sentinel).
                const changes = cst.assertNextChangesEqual({
                    cursor,
                    expectedChanges: [
                        expectedInsert(dbA, 1, sharedCollName),
                        {operationType: "endOfTransaction"},
                    ],
                });
                assert.eq(changes[1].lsid.id, lsid.id, "unexpected lsid", {changes, lsid});
                assert.eq(changes[1].txnNumber, txnNumber, "unexpected txnNumber", {
                    changes,
                    txnNumber,
                });
                assert.eq(
                    timestampCmp(changes[1].clusterTime, later),
                    0,
                    "unexpected clusterTime",
                    {
                        changes,
                        later,
                    },
                );
            });
        },
    );
});
