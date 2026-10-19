// Tests resuming a change stream from an event inside a multi-document transaction. All events of a
// transaction share the same clusterTime but have distinct txnOpIndex values, so resuming from a
// token in the middle of the transaction must skip the transaction's earlier events (same
// clusterTime, but before the resume point) and return only the events after the resume point.
//
// @tags: [
//   uses_change_streams,
//   uses_transactions,
// ]
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    ChangeStreamWatchMode,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("resuming a change stream from inside a transaction", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = jsTestName();

    let coll;

    // Runs a transaction with two updates to the same document and returns the resume tokens of the
    // two resulting change events, in order.
    function captureTransactionEventTokens(cst) {
        const session = db.getMongo().startSession();
        const sessionColl = session.getDatabase(testDB.getName()).getCollection(collName);
        const cursor = cst.getChangeStream({
            watchMode: ChangeStreamWatchMode.kCollection,
            coll: collName,
        });
        try {
            session.startTransaction();
            assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {v: 1}}));
            assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {v: 2}}));
            assert.commandWorked(session.commitTransaction_forTesting());

            const tokens = [];
            for (let i = 0; i < 2; i++) {
                const event = cst.getOneChange(cursor);
                assert.eq(event.operationType, "update", event);
                tokens.push(event._id);
            }
            return tokens;
        } finally {
            session.endSession();
        }
    }

    // Opens a collection change stream that resumes with 'startAfter' from the given token.
    function resumeFrom(cst, token) {
        return cst.startWatchingChanges({
            pipeline: [{$changeStream: {startAfter: token}}],
            collection: collName,
            // Use a batch size of 0 so that the event is not returned in the first batch, which
            // cst.getOneChange() would ignore.
            aggregateOptions: {cursor: {batchSize: 0}},
        });
    }

    beforeEach(function () {
        coll = assertDropAndRecreateCollection(testDB, collName);
    });

    afterEach(function () {
        assertDropCollection(testDB, collName);
    });

    it("skips the transaction events that precede the resume point", function () {
        assert.commandWorked(coll.insert({_id: 1, v: 0}));
        withChangeStreamTest(testDB, (cst) => {
            const tokens = captureTransactionEventTokens(cst);

            // The two transaction events share a clusterTime; the first precedes the resume point
            // when resuming from the second, so it must be skipped and never returned.
            const cursor = resumeFrom(cst, tokens[1]);
            assert.commandWorked(coll.insert({_id: 2}));
            const event = cst.getOneChange(cursor);
            assert.eq(event.operationType, "insert", event);
            assert.docEq(event.documentKey, {_id: 2}, event);
        });
    });

    it("returns the transaction events that follow the resume point", function () {
        assert.commandWorked(coll.insert({_id: 1, v: 0}));
        withChangeStreamTest(testDB, (cst) => {
            const tokens = captureTransactionEventTokens(cst);

            // Resuming from the first transaction event must still return the second one, which has
            // the same clusterTime but a later txnOpIndex.
            const cursor = resumeFrom(cst, tokens[0]);
            const event = cst.getOneChange(cursor);
            assert.eq(event.operationType, "update", event);
            assert.docEq(event.updateDescription.updatedFields, {v: 2}, event);
        });
    });
});
