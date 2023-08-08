/**
 * Tests that a change stream will correctly generate endOfTransaction event for unprepared
 * transactions.
 * @tags: [
 *   uses_transactions,
 *   requires_fcv_71,
 *   requires_majority_read_concern,
 *   requires_snapshot_read,
 *   featureFlagEndOfTransactionChangeEvent,
 * ]
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {assertEndOfTransaction, ChangeStreamTest} from "jstests/libs/change_stream_util.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const otherCollName = "change_stream_end_of_transaction_2";
const coll = assertDropAndRecreateCollection(db, "change_stream_end_of_transaction");
assertDropAndRecreateCollection(db, otherCollName);

const otherDbName = "change_stream_end_of_transaction_db";
const otherDbCollName = "someColl";
assertDropAndRecreateCollection(db.getSiblingDB(otherDbName), otherDbCollName);

let cst = new ChangeStreamTest(db);
let collChangeStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {showExpandedEvents: true}},
        {$project: {"lsid.uid": 0, "operationDescription.lsid.uid": 0}}
    ],
    collection: coll,
    doNotModifyInPassthroughs: true
});

let dbChangeStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {showExpandedEvents: true}},
        {$project: {"lsid.uid": 0, "operationDescription.lsid.uid": 0}}
    ],
    collection: 1,
    doNotModifyInPassthroughs: true
});

const sessionOptions = {
    causalConsistency: false
};
const txnOptions = {
    readConcern: {level: "snapshot"},
    writeConcern: {w: "majority"}
};

const session = db.getMongo().startSession(sessionOptions);

// Create these variables before starting the transaction. In sharded passthroughs, accessing
// db[collname] may attempt to implicitly shard the collection, which is not allowed in a txn.
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb[coll.getName()];
const sessionOtherColl = sessionDb[otherCollName];
const sessionOtherDbColl = session.getDatabase(otherDbName)[otherDbCollName];

const txnNumbers = [];

// First transaction only affects main collection
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.insert({_id: 1, a: 0}));
}, txnOptions);
txnNumbers.push(session.getTxnNumber_forTesting());

// Second transaction only affects other collection
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionOtherColl.insert({_id: 2, a: 0}));
}, txnOptions);
txnNumbers.push(session.getTxnNumber_forTesting());

// Third transaction affects both collections
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.insert({_id: 3, a: 0}));
    assert.commandWorked(sessionOtherColl.insert({_id: 3, a: 0}));
}, txnOptions);
txnNumbers.push(session.getTxnNumber_forTesting());

// Forth transaction affects the other db
withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionOtherDbColl.insert({_id: 4, a: 0}));
}, txnOptions);
txnNumbers.push(session.getTxnNumber_forTesting());

// Drop the collection. This will trigger an "invalidate" event at the end of the collection-wide
// stream.
assert.commandWorked(db.runCommand({drop: coll.getName()}));

function insertEvent(collName, txnId) {
    const docId = txnId + 1;
    return {
        documentKey: {_id: docId},
        fullDocument: {_id: docId, a: 0},
        ns: {db: db.getName(), coll: collName},
        operationType: "insert",
        lsid: session.getSessionId(),
        txnNumber: txnNumbers[txnId],
    };
}

function endOfTransactionEvent(txnId) {
    return {
        operationType: "endOfTransaction",
        operationDescription: {
            lsid: session.getSessionId(),
            txnNumber: txnNumbers[txnId],
        },
        lsid: session.getSessionId(),
        txnNumber: txnNumbers[txnId],
    };
}

function dropEvent(collName) {
    return {
        operationType: "drop",
        ns: {db: db.getName(), coll: collName},
    };
}

// Define the set of changes expected for the single-collection case per the operations above.
const expectedChangesColl = [
    insertEvent(coll.getName(), 0),
    endOfTransactionEvent(0),
    insertEvent(coll.getName(), 2),
    endOfTransactionEvent(2),
    dropEvent(coll.getName()),
    {operationType: "invalidate"},
];

const expectedChangesDb = [
    insertEvent(coll.getName(), 0),
    endOfTransactionEvent(0),
    insertEvent(otherCollName, 1),
    endOfTransactionEvent(1),
    insertEvent(coll.getName(), 2),
    insertEvent(otherCollName, 2),
    endOfTransactionEvent(2),
    dropEvent(coll.getName()),
];

// If we are running in a sharded passthrough, then this may have been a multi-shard transaction.
// Change streams will interleave the txn events from across the shards in (clusterTime, txnOpIndex)
// order, and so may not reflect the ordering of writes in the test. We thus verify that exactly the
// expected set of events are observed, but we relax the ordering requirements.
function assertNextChangesEqual({cursor, expectedChanges, expectInvalidate}) {
    const assertEqualFunc = FixtureHelpers.isMongos(db) ? cst.assertNextChangesEqualUnordered
                                                        : cst.assertNextChangesEqual;
    return assertEqualFunc(
        {cursor: cursor, expectedChanges: expectedChanges, expectInvalidate: expectInvalidate});
}

const collChanges = assertNextChangesEqual(
    {cursor: collChangeStream, expectedChanges: expectedChangesColl, expectInvalidate: true});
assertEndOfTransaction(collChanges);
const dbChanges = assertNextChangesEqual(
    {cursor: dbChangeStream, expectedChanges: expectedChangesDb, expectInvalidate: false});
assertEndOfTransaction(dbChanges);

cst.cleanUp();
