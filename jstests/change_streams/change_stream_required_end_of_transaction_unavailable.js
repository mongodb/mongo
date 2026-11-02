/**
 * Verifies that a change stream opened with 'showEndOfTransaction: "required"' fails with a
 * non-resumable ChangeStreamFatalError when it reads an unprepared multi-document transaction for
 * which no endOfTransaction event can be synthesized.
 *
 * The stream is opened while the end-of-transaction write path is enabled. The
 * 'allowEndOfTransaction' server parameter is then disabled and a transaction is committed; its
 * final applyOps entry therefore carries no 'allowEndOfTransaction' marker, so no event can ever be
 * synthesized for it. With the parameter restored, the next getMore fails after the transaction has
 * been unspooled (the failure is only raised once the whole transaction has been read, because
 * deciding whether the transaction touches the watched namespace needs the complete set of affected
 * namespaces).
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_change_streams,
 *   uses_transactions,
 *   featureFlagEndOfTransactionChangeEventSynthesis,
 *   # The failure is raised by the replica-set applyOps synthesis path on mongod; sharded streams
 *   # synthesize the event on the router instead. Exclude every mongos-based passthrough.
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    ChangeStreamTest,
    isEndOfTransactionUsable,
    withSession,
} from "jstests/libs/query/change_stream_util.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("showEndOfTransaction: required fails when an event cannot be synthesized", function () {
    const collName = jsTestName();

    before(function () {
        assertDropAndRecreateCollection(db, collName);
    });

    after(function () {
        assertDropCollection(db, collName);
    });

    it("fails after unspooling a transaction committed while the parameter was disabled", function () {
        if (!isEndOfTransactionUsable(db)) {
            jsTest.log.info("Skipping test: end-of-transaction change streams are not usable");
            return;
        }

        const cst = new ChangeStreamTest(db);
        try {
            // Open the stream while the write path is enabled, so it resolves to the synthesis
            // behavior and requires an endOfTransaction event for every eligible transaction.
            runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: true}, () => {
                const cursor = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {showEndOfTransaction: "required"}}],
                    collection: collName,
                });

                // Commit a transaction while the parameter is disabled. Its final applyOps entry
                // carries no 'allowEndOfTransaction' marker, so no event can ever be synthesized
                // for it. The helper restores the parameter when it returns.
                runWithParamsAllNonConfigNodes(db, {allowEndOfTransaction: false}, () => {
                    withSession(db.getMongo(), (session) => {
                        const sessionColl = session
                            .getDatabase(db.getName())
                            .getCollection(collName);
                        session.startTransaction();
                        assert.commandWorked(sessionColl.insertOne({_id: 1}, {session}));
                        assert.commandWorked(sessionColl.insertOne({_id: 2}, {session}));
                        assert.commandWorked(session.commitTransaction_forTesting());
                    });
                });

                // The stream returns the transaction's own events, then fails once the whole
                // transaction has been unspooled because the required endOfTransaction event
                // cannot be produced.
                const changes = cst.getNextChanges(cursor, 2);
                assertChangeStreamEventEq(changes[0], {
                    operationType: "insert",
                    ns: {db: db.getName(), coll: collName},
                    documentKey: {_id: 1},
                    fullDocument: {_id: 1},
                });
                assertChangeStreamEventEq(changes[1], {
                    operationType: "insert",
                    ns: {db: db.getName(), coll: collName},
                    documentKey: {_id: 2},
                    fullDocument: {_id: 2},
                });

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
        } finally {
            cst.cleanUp();
        }
    });
});
