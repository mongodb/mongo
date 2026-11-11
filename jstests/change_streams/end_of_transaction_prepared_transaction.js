/**
 * Confirms that a *prepared* transaction committed on a replica set produces an endOfTransaction
 * change event, including for a stream opened with 'showEndOfTransaction: "required"'.
 *
 * A replica-set transaction has two commit shapes:
 *   - unprepared: the commit is a final 'applyOps' entry, from which the unwind stage synthesizes
 *     the endOfTransaction event (the control case below);
 *   - prepared: the commit is a 'commitTransaction' oplog entry. The unwind stage unspools the
 *     transaction's operations from that entry and synthesizes an endOfTransaction event for it
 *     when the entry carries the 'allowEndOfTransaction' marker.
 *
 * The marker is written on the final 'applyOps' entry of an unprepared transaction and on the
 * 'commitTransaction' entry of a prepared transaction, while the write path is enabled at commit
 * time.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_change_streams,
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   # The synthesis happens in the replica-set unwind stage on mongod; sharded streams synthesize
 *   # on the router. Exclude every mongos-based passthrough.
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe} from "jstests/libs/mochalite.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    assertChangeStreamEventEq,
    itWhenEndOfTransactionIsUsable,
    withChangeStreamTest,
    withSession,
} from "jstests/libs/query/change_stream_util.js";

describe("endOfTransaction for a prepared replica-set transaction", function () {
    const collName = jsTestName();

    before(function () {
        assertDropAndRecreateCollection(db, collName);
    });

    after(function () {
        assertDropCollection(db, collName);
    });

    function openRequiredStream(cst) {
        return cst.startWatchingChanges({
            pipeline: [{$changeStream: {showEndOfTransaction: "required"}}],
            collection: collName,
        });
    }

    function insertSentinel(id) {
        assert.commandWorked(db.getCollection(collName).insertOne({_id: id}));
    }

    function assertInsert(changes, index, id) {
        assertChangeStreamEventEq(changes[index], {
            operationType: "insert",
            ns: {db: db.getName(), coll: collName},
            documentKey: {_id: id},
            fullDocument: {_id: id},
        });
    }

    itWhenEndOfTransactionIsUsable(
        db,
        "control: an unprepared transaction emits an endOfTransaction event",
        function () {
            withChangeStreamTest(db, (cst) => {
                const cursor = openRequiredStream(cst);

                withSession(db.getMongo(), (session) => {
                    const sessionColl = session.getDatabase(db.getName()).getCollection(collName);
                    session.startTransaction();
                    assert.commandWorked(sessionColl.insertOne({_id: 1}, {session}));
                    assert.commandWorked(sessionColl.insertOne({_id: 2}, {session}));
                    assert.commandWorked(session.commitTransaction_forTesting());
                });
                insertSentinel("sentinel-unprepared");

                // The transaction's two inserts are followed by its endOfTransaction event and then
                // the sentinel insert, which bounds the stream.
                const changes = cst.getNextChanges(cursor, 4);
                assertInsert(changes, 0, 1);
                assertInsert(changes, 1, 2);
                assert.eq(
                    changes[2].operationType,
                    "endOfTransaction",
                    "an unprepared transaction should emit an endOfTransaction event",
                    {changes},
                );
                assertInsert(changes, 3, "sentinel-unprepared");
            });
        },
    );

    itWhenEndOfTransactionIsUsable(
        db,
        "a prepared transaction emits an endOfTransaction event",
        function () {
            withChangeStreamTest(db, (cst) => {
                const cursor = openRequiredStream(cst);

                withSession(db.getMongo(), (session) => {
                    const sessionColl = session.getDatabase(db.getName()).getCollection(collName);
                    session.startTransaction();
                    assert.commandWorked(sessionColl.insertOne({_id: 11}, {session}));
                    assert.commandWorked(sessionColl.insertOne({_id: 12}, {session}));
                    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                    assert.commandWorked(
                        PrepareHelpers.commitTransaction(session, prepareTimestamp),
                    );
                });
                insertSentinel("sentinel-prepared");

                // The prepared transaction's two inserts are followed by its endOfTransaction event
                // and then the sentinel insert, which bounds the stream.
                const changes = cst.getNextChanges(cursor, 4);
                assertInsert(changes, 0, 11);
                assertInsert(changes, 1, 12);
                assert.eq(
                    changes[2].operationType,
                    "endOfTransaction",
                    "a prepared transaction should emit an endOfTransaction event",
                    {changes},
                );
                assertInsert(changes, 3, "sentinel-prepared");
            });
        },
    );

    itWhenEndOfTransactionIsUsable(
        db,
        "a required stream fails when a prepared transaction has no marker",
        function () {
            // A shard-local stream (a stream opened directly against a shard's mongod, e.g. by a
            // mongot-like client) runs entirely on one shard and cannot observe the other participants
            // of a multi-shard transaction, so it treats 'showEndOfTransaction: "required"' as
            // '"whenAvailable"' and never fails for an ineligible transaction. Only a replica-set or
            // router stream enforces 'required', so the failure below only applies when the test is not
            // connected directly to a shard. See isShardLocalStream() in change_stream_helpers.cpp.
            if (TestData.connectedDirectlyToShard) {
                jsTest.log.info(
                    "Skipping test: a shard-local stream treats 'required' as 'whenAvailable'",
                );
                return;
            }

            withChangeStreamTest(db, (cst) => {
                // Open the stream while the write path is enabled, so it resolves to the synthesis
                // behavior and requires an endOfTransaction event for every eligible transaction.
                runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: true}, () => {
                    const cursor = openRequiredStream(cst);

                    // Commit a prepared transaction while the parameter is disabled. Its
                    // 'commitTransaction' entry carries no 'allowEndOfTransaction' marker, so no event
                    // can ever be synthesized for it. The helper restores the parameter when it
                    // returns.
                    runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: false}, () => {
                        withSession(db.getMongo(), (session) => {
                            const sessionColl = session
                                .getDatabase(db.getName())
                                .getCollection(collName);
                            session.startTransaction();
                            assert.commandWorked(sessionColl.insertOne({_id: 21}, {session}));
                            assert.commandWorked(sessionColl.insertOne({_id: 22}, {session}));
                            const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                            assert.commandWorked(
                                PrepareHelpers.commitTransaction(session, prepareTimestamp),
                            );
                        });
                    });

                    // The stream returns the transaction's own events, then fails once the whole
                    // transaction has been unspooled because the required endOfTransaction event
                    // cannot be produced.
                    const changes = cst.getNextChanges(cursor, 2);
                    assertInsert(changes, 0, 21);
                    assertInsert(changes, 1, 22);

                    const error = assert.throwsWithCode(
                        () => cst.getNextBatch(cursor),
                        ErrorCodes.ChangeStreamFatalError,
                    );
                    assert(
                        error.message.includes("no endOfTransaction event could be generated"),
                        "unexpected error message for the unproducible endOfTransaction event",
                        {error},
                    );
                });
            });
        },
    );
});
