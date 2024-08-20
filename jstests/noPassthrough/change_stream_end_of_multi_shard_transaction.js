
/**
 * Tests that a change stream will correctly generate endOfTransaction event for prepared
 * transactions.
 * @tags: [
 *   uses_transactions,
 *   requires_fcv_71,
 * ]
 */

import {assertEndOfTransaction, ChangeStreamTest} from "jstests/libs/change_stream_util.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        setParameter: {
            writePeriodicNoops: true,
            periodicNoopIntervalSecs: 1,
            featureFlagEndOfTransactionChangeEvent: true
        }
    }
});

const db = st.s.getDB(jsTestName());

const collName = "change_stream_end_of_transaction";
assertDropAndRecreateCollection(db, collName);
const coll2Name = "change_stream_end_of_transaction_2";
assertDropAndRecreateCollection(db, coll2Name);

st.shardColl(db[collName], {_id: 1}, {_id: 10}, {_id: 10});
st.shardColl(db[coll2Name], {_id: 1}, {_id: 10}, {_id: 10});

const sessionOptions = {
    causalConsistency: false
};

const session = db.getMongo().startSession(sessionOptions);

let cst = new ChangeStreamTest(db);
let collChangeStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {showExpandedEvents: true}},
        {$project: {"lsid.uid": 0, "operationDescription.lsid.uid": 0}}
    ],
    collection: db[collName],
    doNotModifyInPassthroughs: true
});

let collSessionChangeStream = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {showExpandedEvents: true}},
        {$match: {"lsid.id": session.getSessionId().id}},
        {$project: {"lsid.uid": 0, "operationDescription.lsid.uid": 0}}
    ],
    collection: db[collName],
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

// Create these variables before starting the transaction. In sharded passthroughs, accessing
// db[collname] may attempt to implicitly shard the collection, which is not allowed in a txn.
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb[collName];
const sessionColl2 = sessionDb[coll2Name];

const txnOptions = {
    readConcern: {level: "snapshot"},
    writeConcern: {w: "majority"}
};

const txnNumbers = [];

// First transaction only affects the first collection
session.startTransaction(txnOptions);
assert.commandWorked(sessionColl.insert({_id: 1, a: 0}));
assert.commandWorked(sessionColl.insert({_id: 11, a: 0}));
session.commitTransaction_forTesting();
txnNumbers.push(session.getTxnNumber_forTesting());

// Second transaction only affects the other collection
session.startTransaction(txnOptions);
assert.commandWorked(sessionColl2.insert({_id: 2, a: 0}));
assert.commandWorked(sessionColl2.insert({_id: 12, a: 0}));
session.commitTransaction_forTesting();
txnNumbers.push(session.getTxnNumber_forTesting());

// Third transaction affects both collections
session.startTransaction(txnOptions);
assert.commandWorked(sessionColl.insert({_id: 3, a: 0}));
assert.commandWorked(sessionColl.insert({_id: 13, a: 0}));
assert.commandWorked(sessionColl2.insert({_id: 4, a: 0}));
assert.commandWorked(sessionColl2.insert({_id: 14, a: 0}));
session.commitTransaction_forTesting();
txnNumbers.push(session.getTxnNumber_forTesting());

const dbName = db.getName();
// Drop the database. This will trigger an "invalidate" event for all streams.
assert.commandWorked(db.runCommand({dropDatabase: 1}));

function insertEvent(collName, txnId, docId) {
    return {
        documentKey: {_id: docId},
        fullDocument: {_id: docId, a: 0},
        ns: {db: dbName, coll: collName},
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
        ns: {db: dbName, coll: collName},
    };
}

const expectedChangesCollSession = [
    insertEvent(collName, 0, 1),
    insertEvent(collName, 0, 11),
    endOfTransactionEvent(0),
    insertEvent(collName, 2, 3),
    insertEvent(collName, 2, 13),
    endOfTransactionEvent(2),
];

const expectedChangesColl = expectedChangesCollSession.concat([
    dropEvent(collName),
    {operationType: "invalidate"},
]);

const expectedChangesDb = [
    insertEvent(collName, 0, 1),
    insertEvent(collName, 0, 11),
    endOfTransactionEvent(0),
    insertEvent(coll2Name, 1, 2),
    insertEvent(coll2Name, 1, 12),
    endOfTransactionEvent(1),
    insertEvent(collName, 2, 3),
    insertEvent(collName, 2, 13),
    insertEvent(coll2Name, 2, 4),
    insertEvent(coll2Name, 2, 14),
    endOfTransactionEvent(2),
    dropEvent(collName),
    dropEvent(coll2Name),
    {operationType: "dropDatabase", ns: {db: dbName}},
    {operationType: "invalidate"},
];

const collChanges = cst.assertNextChangesEqualUnordered(
    {cursor: collChangeStream, expectedChanges: expectedChangesColl, expectInvalidate: true});
assertEndOfTransaction(collChanges);

const collSessionChanges = cst.assertNextChangesEqualUnordered({
    cursor: collSessionChangeStream,
    expectedChanges: expectedChangesCollSession,
    expectInvalidate: false
});
assertEndOfTransaction(collSessionChanges);

const dbChanges = cst.assertNextChangesEqualUnordered(
    {cursor: dbChangeStream, expectedChanges: expectedChangesDb, expectInvalidate: true});
assertEndOfTransaction(dbChanges);

cst.cleanUp();
st.stop();
