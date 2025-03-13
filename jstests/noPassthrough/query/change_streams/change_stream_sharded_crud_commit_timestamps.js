/**
 * Tests that change stream events for CRUD events in distributed transactions contain the
 * 'commitTimestamp' field.
 */

import {withTxnAndAutoRetryOnMongos} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {assertChangeStreamEventEq} from "jstests/libs/query/change_stream_util.js";
import {assertNoChanges} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {isTimestamp} from "jstests/libs/timestamp_util.js";

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        setParameter: {
            writePeriodicNoops: true,
            periodicNoopIntervalSecs: 1,
        }
    }
});

// Pads a key with leading zeros so we can sort it by numeric (chronologic) order.
function padKey(value, length = 4) {
    const s = String(value);
    return '0'.repeat(length - s.length) + s;
}

function assertNextEvents(cursor, expectedEvents, expectCommitTimestamp) {
    assert(expectedEvents.length);

    // Fetch as many events as expected.
    let actualEvents = [];
    for (let i = 0; i < expectedEvents.length; ++i) {
        assert.soon(() => cursor.hasNext());
        const changeDoc = cursor.next();
        actualEvents.push(changeDoc);
    }
    assert.eq(expectedEvents.length, actualEvents.length, "expecting equal number of events");

    // Sort actual events by operationType, then documentKey.
    // This is necessary because in a multi-shard transactions, there is no guaranteed order between
    // the operations on different shards. 'endOfTransaction' must always be the final event. An
    // update operation for a shard key change consists of a 'delete' event on one shard and an
    // 'insert' event on another shard. These can appear in non-deterministic order, and we sort
    // them deterministically here.
    actualEvents.sort((l, r) => {
        const opOrders = ['insert', 'delete', 'endOfTransaction'];
        const lIndex = opOrders.indexOf(l.operationType);
        const rIndex = opOrders.indexOf(r.operationType);
        if (lIndex !== rIndex) {
            return lIndex < rIndex ? -1 : 1;
        }
        assert(l.documentKey._id !== r.documentKey._id);
        return l.documentKey._id < r.documentKey._id ? -1 : 1;
    });

    let commitTimestamp = null;
    for (let i = 0; i < expectedEvents.length; ++i) {
        const changeDoc = actualEvents[i];
        assertChangeStreamEventEq(changeDoc, expectedEvents[i]);
        if (!expectCommitTimestamp || changeDoc.operationType === 'endOfTransaction') {
            continue;
        }
        assert(changeDoc.hasOwnProperty("commitTimestamp"),
               "expecting doc to have a 'commitTimestamp' field",
               {changeDoc});
        assert(isTimestamp(changeDoc["commitTimestamp"]),
               "expecting 'commitTimestamp' field to be a timestamp",
               {changeDoc});
        if (commitTimestamp === null) {
            commitTimestamp = changeDoc.commitTimestamp;
        } else {
            assert.eq(commitTimestamp,
                      changeDoc["commitTimestamp"],
                      "expecting equal commitTimestamps",
                      {commitTimestamp, changeDoc});
        }

        // Commit timestamp must be before clusterTime.
        assert(timestampCmp(commitTimestamp, changeDoc.clusterTime) < 0,
               "expecting commitTimestamp to be before clusterTime",
               {commitTimestamp, changeDoc});
    }
    assertNoChanges(cursor);
}

// Create database with a sharded collection (2 shards).
const db = st.s.getDB(jsTestName());
assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

const collName = "change_stream_commit_timestamp";
assertDropAndRecreateCollection(db, collName);
const kNsName = jsTestName() + "." + collName;
const ns = {
    db: jsTestName(),
    coll: collName
};

// Shard and split the collection.
assert.commandWorked(st.s0.adminCommand({shardCollection: kNsName, key: {shard: 1}}));
assert.commandWorked(st.s0.adminCommand({split: kNsName, middle: {shard: 0}}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: kNsName, find: {shard: -1}, to: st["shard0"].shardName}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: kNsName, find: {shard: 1}, to: st["shard1"].shardName}));

// Create changestream on the target database.
const cursor = db.watch([], {showExpandedEvents: true});

// Start session.
const sessionOptions = {
    causalConsistency: true
};

const txnOpts = {
    writeConcern: {w: "majority"}
};

// Transaction with two individual inserts on the same shard.
jsTestLog("Testing individual operations in transaction - same shard");
let session = db.getMongo().startSession(sessionOptions);
let sessionDb = session.getDatabase(jsTestName());
let sessionColl = sessionDb[collName];

let expectedEvents;
withTxnAndAutoRetryOnMongos(session, () => {
    expectedEvents = [];
    for (let i = 0; i < 10; ++i) {
        const doc = {_id: "single-same-" + padKey(i), shard: -1};
        assert.commandWorked(sessionColl.insert(doc));
        expectedEvents.push({operationType: "insert", ns, documentKey: doc, fullDocument: doc});
    }
    if (FeatureFlagUtil.isEnabled(db, "EndOfTransactionChangeEvent")) {
        expectedEvents.push({operationType: "endOfTransaction"});
    }
}, txnOpts);
assertNextEvents(cursor, expectedEvents, false /* expectCommitTimestamp */);

// Transaction with two individual inserts on different shards.
jsTestLog("Testing individual operations in transaction - different shards");
session = db.getMongo().startSession(sessionOptions);
sessionDb = session.getDatabase(jsTestName());
sessionColl = sessionDb[collName];

withTxnAndAutoRetryOnMongos(session, () => {
    assert.commandWorked(sessionColl.insert({_id: "single-diff-1", shard: -1}));
    assert.commandWorked(sessionColl.insert({_id: "single-diff-2", shard: 1}));
    assert.commandWorked(sessionColl.remove({_id: "single-diff-1", shard: -1}));
}, txnOpts);

expectedEvents = [
    {
        operationType: "insert",
        ns,
        documentKey: {_id: "single-diff-1", shard: -1},
        fullDocument: {_id: "single-diff-1", shard: -1}
    },
    {
        operationType: "insert",
        ns,
        documentKey: {_id: "single-diff-2", shard: 1},
        fullDocument: {_id: "single-diff-2", shard: 1}
    },
    {operationType: "delete", ns, documentKey: {_id: "single-diff-1", shard: -1}},
];
if (FeatureFlagUtil.isEnabled(db, "EndOfTransactionChangeEvent")) {
    expectedEvents.push({operationType: "endOfTransaction"});
}
assertNextEvents(cursor, expectedEvents, false /* expectCommitTimestamp */);

// Shard key change.
jsTestLog("Testing shard key change");
// Insert the target document before the transaction, because inserting & updating the document in
// the same transaction would be merged into a single insert.
assert.commandWorked(db[collName].insert({_id: "shard-key-1", shard: 1}));
expectedEvents = [
    {
        operationType: "insert",
        ns,
        documentKey: {_id: "shard-key-1", shard: 1},
        fullDocument: {_id: "shard-key-1", shard: 1}
    },
];
assertNextEvents(cursor, expectedEvents, false /* expectCommitTimestamp */);
session = db.getMongo().startSession(sessionOptions);
sessionDb = session.getDatabase(jsTestName());
sessionColl = sessionDb[collName];

withTxnAndAutoRetryOnMongos(session, () => {
    expectedEvents = [
        {
            operationType: "insert",
            ns,
            documentKey: {_id: "shard-key-1", shard: -1},
            fullDocument: {_id: "shard-key-1", shard: -1, updated: 1}
        },
        {operationType: "delete", ns, documentKey: {_id: "shard-key-1", shard: 1}},
    ];
    assert.commandWorked(
        sessionColl.update({_id: "shard-key-1", shard: 1}, {$set: {shard: -1, updated: 1}}));
    if (FeatureFlagUtil.isEnabled(db, "EndOfTransactionChangeEvent")) {
        expectedEvents.push({operationType: "endOfTransaction"});
    }
}, txnOpts);
assertNextEvents(cursor, expectedEvents, true /* expectCommitTimestamp */);

// Transaction with batch inserts on different shards.
jsTestLog("Testing batch inserts in transaction");
session = db.getMongo().startSession(sessionOptions);
sessionDb = session.getDatabase(jsTestName());
sessionColl = sessionDb[collName];

withTxnAndAutoRetryOnMongos(session, () => {
    expectedEvents = [];
    let docs = [];
    for (let i = 0; i < 20; ++i) {
        const doc = {_id: "batch-" + padKey(i), shard: i < 10 ? -1 : 1};
        docs.push(doc);
        expectedEvents.push({operationType: "insert", ns, documentKey: doc, fullDocument: doc});
        if (docs.length === 10) {
            assert.commandWorked(sessionColl.insert(docs, {ordered: true}));
            docs = [];
        }
    }
    if (FeatureFlagUtil.isEnabled(db, "EndOfTransactionChangeEvent")) {
        expectedEvents.push({operationType: "endOfTransaction"});
    }
}, txnOpts);
assertNextEvents(cursor, expectedEvents, true /* expectCommitTimestamp */);

cursor.close();

st.stop();
