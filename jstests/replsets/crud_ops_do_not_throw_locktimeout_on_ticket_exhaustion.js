/**
 * Test ensures that CRUD operations that time out because they cannot acquire a ticket do not
 * return a LockTimeout.
 *
 * @tags: [
 *   requires_fcv_70,
 *   uses_transactions,
 *   uses_prepare_transaction,
 * ]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load('jstests/libs/parallel_shell_helpers.js');

// We set the number of tickets to be a small value in order to avoid needing to spawn a large
// number of threads to exhaust all of the available ones.
const kNumWriteTickets = 5;
const kNumReadTickets = 5;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // This test requires a fixed ticket pool size.
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            wiredTigerConcurrentWriteTransactions: kNumWriteTickets,
            wiredTigerConcurrentReadTransactions: kNumReadTickets,
            logComponentVerbosity: tojson({storage: 1, command: 2})
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const db = primary.getDB(dbName);
const coll = db[jsTestName()];
const otherCollName = jsTestName() + "_other";

const doc = {
    _id: 1
};
assert.commandWorked(coll.insert(doc));
assert.commandWorked(db[otherCollName].insert(doc));

jsTestLog("Starting transaction");
const session = primary.startSession();
const sessionDb = session.getDatabase(dbName);

session.startTransaction();
assert.commandWorked(sessionDb[otherCollName].update(doc, {$set: {a: 1}}));

jsTestLog("Preparing transaction so readers and writers block holding a ticket");
PrepareHelpers.prepareTransaction(session);

const threads = [];
jsTestLog(`Starting ${kNumReadTickets} readers`);
for (let i = 0; i < kNumReadTickets; ++i) {
    const thread =
        startParallelShell(funWithArgs(function(dbName, collName) {
                               assert.commandWorked(db.getSiblingDB(dbName).runCommand(
                                   {"find": collName, readConcern: {level: "linearizable"}}));
                           }, db.getName(), otherCollName), primary.port);

    threads.push(thread);
}

jsTestLog(`Starting ${kNumWriteTickets} writers`);
for (let i = 0; i < kNumWriteTickets; ++i) {
    const thread = startParallelShell(
        funWithArgs(function(dbName, collName, doc) {
            assert.commandWorked(db.getSiblingDB(dbName)[collName].update(doc, {$set: {a: 2}}));
        }, db.getName(), otherCollName, doc), primary.port);

    threads.push(thread);
}

jsTestLog("Waiting for reads and writes to block on prepare conflicts");

assert.soon(
    () => {
        const commandObj = {"currentOp": 1};
        const queryObj = {
            "$and": [
                {"$or": [{"op": "query"}, {"op": "update"}]},
                {"ns": dbName + "." + otherCollName},
                {"prepareReadConflicts": {"$gt": 0}}
            ]
        };
        Object.extend(commandObj, queryObj);
        const ops = db.adminCommand(commandObj);
        return ops.inprog.length === (kNumReadTickets + kNumWriteTickets);
    },
    () => {
        return "Didn't find enough commands running: " + tojson(db.currentOp());
    });

const failureTimeoutMS = 1 * 1000;

// Each of the following operations should time out trying to acquire a read or write ticket. The
// tickets are all held by the readers and writers started above.
jsTestLog("Testing CRUD op timeouts");

assert.commandFailedWithCode(
    db.runCommand({insert: coll.getName(), documents: [{_id: 2}], maxTimeMS: failureTimeoutMS}),
    ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), maxTimeMS: failureTimeoutMS}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates: [{q: doc, u: {$set: {b: 1}}}],
    maxTimeMS: failureTimeoutMS
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(
    db.runCommand(
        {delete: coll.getName(), deletes: [{q: doc, limit: 1}], maxTimeMS: failureTimeoutMS}),
    ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    findAndModify: coll.getName(),
    query: {q: doc},
    update: {$set: {b: 2}},
    maxTimeMS: failureTimeoutMS
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    findAndModify: coll.getName(),
    query: {q: doc},
    remove: true,
    maxTimeMS: failureTimeoutMS
}),
                             ErrorCodes.MaxTimeMSExpired);

jsTestLog("Aborting transaction");
assert.commandWorked(session.abortTransaction_forTesting());

jsTestLog("Waiting for threads to join");
for (let joinThread of threads) {
    joinThread();
}

rst.stopSet();
})();
