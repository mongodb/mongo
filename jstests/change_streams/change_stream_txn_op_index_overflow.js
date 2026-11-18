/**
 * Tests that a change stream fails with ChangeStreamFatalError when it encounters a transaction
 * operation whose 'txnOpIndex' is INT32_MAX or more, since such an index cannot be encoded in a
 * resume token.
 *
 * @tags: [
 *   uses_change_streams,
 *   uses_transactions,
 *   requires_majority_read_concern,
 *   requires_snapshot_read,
 *   # The fail point only exists on the latest binary, so exclude mixed-version suites.
 *   requires_fcv_91,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {withFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ServerStatusMetrics} from "jstests/libs/query/change_stream_metrics_util.js";
import {
    assertEndOfTransaction,
    itWhenEndOfTransactionIsUsable,
    withChangeStreamTest,
    withSession,
} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const kInt32Max = 2147483647;
const kCollName = jsTestName();
const kFailPointName = "changeStreamUnwindTransactionInitialTxnOpIndex";

describe("change stream transaction txnOpIndex limit", function () {
    let coll;

    before(function () {
        coll = assertDropAndRecreateCollection(db, kCollName);
    });

    after(function () {
        assertDropCollection(db, kCollName);
    });

    function runTxnInsert(id) {
        withSession(db.getMongo(), (session) => {
            const sessionColl = session.getDatabase(db.getName()).getCollection(kCollName);
            withTxnAndAutoRetryOnMongos(
                session,
                () => {
                    assert.commandWorked(sessionColl.insert({_id: id}));
                },
                {readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}},
            );
        });
    }

    function withInitialTxnOpIndex(index, callback) {
        withFailPointForAllShardsAndMongos({
            conn: db.getMongo(),
            failPointName: kFailPointName,
            data: {index},
            callback,
        });
    }

    it("emits events for a transaction operation at the last encodable index", function () {
        withChangeStreamTest(db, (cst) => {
            const cursor = cst.startWatchingChanges({
                pipeline: [{$changeStream: {}}],
                collection: coll,
            });
            withInitialTxnOpIndex(kInt32Max - 1, () => {
                runTxnInsert(1);
                const event = cst.getOneChange(cursor);
                assert.eq(event.operationType, "insert", event);
                assert.eq(event.documentKey._id, 1, event);
            });
        });
    });

    // Kept separate from the case above so that the other cases still run when the feature flag is
    // off. The feature flag is checked by 'itWhenEndOfTransactionIsUsable'; the server parameter
    // that makes the write path mark transactions for endOfTransaction synthesis is turned on here.
    itWhenEndOfTransactionIsUsable(
        db,
        "emits the endOfTransaction event after an operation at the last encodable index",
        function () {
            withChangeStreamTest(db, (cst) => {
                // The metrics deltas are observed around the transaction. The transaction
                // writes an unsharded collection, so it is a single-shard transaction: its
                // event is synthesized by that shard's transform (the transformed counter)
                // whether the test runs on a replica set or through mongos; the router's
                // synthesis stage is not involved.
                const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(db, () => {
                    runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: true}, () => {
                        const cursor = cst.startWatchingChanges({
                            pipeline: [{$changeStream: {showEndOfTransaction: "required"}}],
                            collection: coll,
                        });
                        withInitialTxnOpIndex(kInt32Max - 1, () => {
                            runTxnInsert(2);
                            const changes = cst.getNextChanges(cursor, 2);
                            assert.eq(changes[0].operationType, "insert", changes);
                            assert.eq(changes[0].documentKey._id, 2, changes);
                            assert.eq(changes[1].operationType, "endOfTransaction", changes);
                            assertEndOfTransaction(changes);
                        });
                    });
                });

                const eot = ServerStatusMetrics.extractEndOfTransactionDelta(delta);
                assert.gte(eot.transformed, 1, {eot});
                if (FixtureHelpers.isMongos(db)) {
                    // The router's synthesis stage is not involved for a single-shard
                    // transaction.
                    assert.eq(eot.emitted, 0, {eot});
                    assert.eq(eot.pbrtBumps, 0, {eot});
                    assert.eq(eot.tracked, 0, {eot});
                }
            });
        },
    );

    for (const index of [kInt32Max, kInt32Max + 10]) {
        it(`fails with ChangeStreamFatalError for txnOpIndex ${index}`, function () {
            withChangeStreamTest(db, (cst) => {
                const cursor = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {}}],
                    collection: coll,
                });
                withInitialTxnOpIndex(index, () => {
                    runTxnInsert(100 + (index % 100));
                    assert.throwsWithCode(
                        () => cst.getOneChange(cursor),
                        ErrorCodes.ChangeStreamFatalError,
                    );
                });
            });
        });
    }
});
