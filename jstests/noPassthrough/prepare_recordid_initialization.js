/**
 * This test reproduces a bug, described in SERVER-58409, where reconstructing a prepared
 * transaction during startup recovery re-uses a RecordId of a deleted document and timestamps it
 * in the past. This generates an out-of-order update chain in WiredTiger and can return wrong
 * results for some reads.
 *
 * Consider the following sequence with durable history:
 * - Set OldestTimestamp 1
 * - Insert RecordId(1) -> A at TimeStamp(10)
 * - Insert RID(2) -> B at TS(20)
 * - Delete RID(2) (B) at TS(30)
 *
 * If we were to restart and initialize the next record id, we'd start issuing new documents at
 * RID(2). Normally this is fine. Any new replicated user writes must be generated with a timestamp
 * larger than 30, so the update chain for RID(2) will remain valid.
 *
 * However, when reconstructing prepared transactions, the prepare timestamp (and thus any following
 * commit timestamp, but not the durable timestamp) may be arbitrarily old. In this example, after
 * initializing the next RID to 2, if we were to reconstruct a prepared transaction from TS(10) that
 * performs an insert on this collection, we'd get the following update chain (from oldest to
 * newest):
 *  - RID(2) => B @ TS(20) -> <tombstone> @ TS(30) -> PreparedInsert @ TS(10)
 *
 * Committing the prepared insert at a value between 10 and 30 results in wrong results/inconsistent
 * data when reading at those timestamps. For example, a reader reading before TS 30 and after TS 10
 * would not see the document at RID(2) even though it should.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication
 * ]
 */
(function() {
"use strict";

TestData.skipEnforceFastCountOnValidate = true;

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/aggregation/extras/utils.js");  // For arrayEq

function incTs(ts) {
    return Timestamp(ts.t, ts.i + 1);
}

let replTest = new ReplSetTest({
    name: "prepare_recordid_initialization",
    nodes: 1,
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojson({storage: {recovery: 2}, transaction: 2}),
            // Set the history window to zero to explicitly control the oldest timestamp. This is
            // necessary to predictably exercise the minimum visible timestamp initialization of
            // collections and indexes across a restart.
            minSnapshotHistoryWindowInSeconds: 0,
        }
    }
});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();
let coll = primary.getDB("test")["foo"];

let origInsertTs = primary.getDB("test").runCommand(
    {insert: "foo", documents: [{_id: 1}], writeConcern: {w: "majority"}})["operationTime"];

// Pin with an arbitrarily small timestamp. Let the rounding tell us where the pin ended up. The
// write to the `mdb_testing.pinned_timestamp` collection is not logged/replayed during replication
// recovery. Repinning across startup happens before replication recovery. Do a majority write for
// predictability of the test.
assert.commandWorked(primary.adminCommand(
    {"pinHistoryReplicated": incTs(origInsertTs), round: true, writeConcern: {w: "majority"}}));

let s1 = primary.startSession();
let s1DB = s1.getDatabase("test");
let s1Coll = s1DB.getCollection("foo");
s1.startTransaction();

assert.commandWorked(s1Coll.insert({_id: 2, prepared: true}));  // RID: 2
let prepTs = PrepareHelpers.prepareTransaction(s1);

assert.commandWorked(coll.insert({_id: 3, cnt: 1}));  // RID: 3
let readCollidingTs = assert.commandWorked(primary.getDB("test").runCommand(
    {insert: "foo", documents: [{_id: 4, cnt: 1}]}))["operationTime"];  // RID: 4
assert.commandWorked(coll.remove({_id: 4}));

// After deleting _id: 4, the highest visible RID will be 3. When reconstructing the prepared insert
// that was previously at RID 2, we should not insert at RID 4. Instead, we will determine that RID
// 4 is not visible and insert at RID 5.
replTest.restart(primary);
primary = replTest.getPrimary();
replTest.awaitLastOpCommitted();

const lsid = s1.getSessionId();
const txnNumber = s1.getTxnNumber_forTesting();

s1 = PrepareHelpers.createSessionWithGivenId(primary, lsid);
s1.setTxnNumber_forTesting(txnNumber);
let sessionDb = s1.getDatabase("test");
assert.commandWorked(sessionDb.adminCommand({
    commitTransaction: 1,
    commitTimestamp: prepTs,
    txnNumber: NumberLong(txnNumber),
    autocommit: false,
}));

let s2 = primary.startSession();
sessionDb = s2.getDatabase("test");
s2.startTransaction({readConcern: {level: "snapshot", atClusterTime: readCollidingTs}});
let docs = sessionDb["foo"].find().showRecordId().toArray();
assert(arrayEq(docs,
               [
                   {"_id": 1, "$recordId": NumberLong(1)},
                   {"_id": 3, "cnt": 1, "$recordId": NumberLong(3)},
                   {"_id": 4, "cnt": 1, "$recordId": NumberLong(4)},
                   {"_id": 2, "prepared": true, "$recordId": NumberLong(5)}
               ]),
       tojson(docs));
assert.commandWorked(s2.commitTransaction_forTesting());

coll = primary.getDB("test")["foo"];
assert.commandWorked(coll.insert({_id: 6}));  // Should not re-use any RecordIds
docs = sessionDb["foo"].find().showRecordId().toArray();
assert(arrayEq(docs,
               [
                   {"_id": 1, "$recordId": NumberLong(1)},
                   {"_id": 3, "cnt": 1, "$recordId": NumberLong(3)},
                   {"_id": 2, "prepared": true, "$recordId": NumberLong(5)},
                   {"_id": 6, "$recordId": NumberLong(6)}
               ]),
       tojson(docs));

replTest.stopSet();
})();
