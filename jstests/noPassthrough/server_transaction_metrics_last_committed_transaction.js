// Tests the 'lastCommittedTransaction' serverStatus section.
// @tags: [uses_transactions, uses_prepare_transaction]
(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}}));
let newDefaultWC = {"w": 1, "wtimeout": 0, "provenance": "customDefault"};

const dbName = "test";
const collName = "coll";

const session = primary.getDB(dbName).getMongo().startSession();
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];
assert.commandWorked(sessionDb.runCommand({create: collName}));

function checkLastCommittedTransaction(operationCount, writeConcern) {
    let res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    assert(res.hasOwnProperty("transactions"), () => tojson(res));
    assert(res.transactions.hasOwnProperty("lastCommittedTransaction"),
           () => tojson(res.transactions));
    assert.eq(operationCount,
              res.transactions.lastCommittedTransaction.operationCount,
              () => tojson(res.transactions));
    if (operationCount === 0) {
        assert.eq(0,
                  res.transactions.lastCommittedTransaction.oplogOperationBytes,
                  () => tojson(res.transactions));
    } else {
        assert.lt(0,
                  res.transactions.lastCommittedTransaction.oplogOperationBytes,
                  () => tojson(res.transactions));
    }
    assert.docEq(writeConcern,
                 res.transactions.lastCommittedTransaction.writeConcern,
                 () => tojson(res.transactions));
}

// Initially the 'lastCommittedTransaction' section is not present.
let res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
assert(res.hasOwnProperty("transactions"), () => tojson(res));
assert(!res.transactions.hasOwnProperty("lastCommittedTransaction"), () => tojson(res));

// Start a transaction. The 'lastCommittedTransaction' section is not yet updated.
session.startTransaction();
assert.commandWorked(sessionColl.insert({}));
res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
assert(res.hasOwnProperty("transactions"), () => tojson(res));
assert(!res.transactions.hasOwnProperty("lastCommittedTransaction"), () => tojson(res));

// Prepare the transaction. The 'lastCommittedTransaction' section is not yet updated.
let prepareTimestampForCommit = PrepareHelpers.prepareTransaction(session);
res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
assert(res.hasOwnProperty("transactions"), () => tojson(res));
assert(!res.transactions.hasOwnProperty("lastCommittedTransaction"), () => tojson(res));

// Commit the transaction. The 'lastCommittedTransaction' section should be updated.
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestampForCommit));
checkLastCommittedTransaction(1, newDefaultWC);

// Check that we are able to exclude 'lastCommittedTransaction'. FTDC uses this to filter out
// the section as it frequently triggers scheme changes.
let filteredRes = assert.commandWorked(
    primary.adminCommand({serverStatus: 1, transactions: {includeLastCommitted: false}}));
assert(!filteredRes.transactions.hasOwnProperty("lastCommittedTransaction"),
       () => tojson(filteredRes));

function runTests(prepare) {
    jsTestLog("Testing server transaction metrics with prepare=" + prepare);

    function commitTransaction() {
        if (prepare) {
            prepareTimestampForCommit = PrepareHelpers.prepareTransaction(session);
            assert.commandWorked(
                PrepareHelpers.commitTransaction(session, prepareTimestampForCommit));
        } else {
            assert.commandWorked(session.commitTransaction_forTesting());
        }
    }

    // Run a transaction with multiple write operations.
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({}));
    assert.commandWorked(sessionColl.insert({}));
    commitTransaction();
    checkLastCommittedTransaction(2, newDefaultWC);

    // Run a read-only transaction.
    session.startTransaction();
    sessionColl.findOne();
    commitTransaction();
    checkLastCommittedTransaction(0, newDefaultWC);

    // Run a transaction with non-default writeConcern.
    session.startTransaction({writeConcern: {w: 1}});
    assert.commandWorked(sessionColl.insert({}));
    commitTransaction();
    checkLastCommittedTransaction(1, {w: 1, wtimeout: 0, provenance: "clientSupplied"});

    // Run a read-only transaction with non-default writeConcern.
    session.startTransaction({writeConcern: {w: "majority"}});
    sessionColl.findOne();
    commitTransaction();
    checkLastCommittedTransaction(0, {w: "majority", wtimeout: 0, provenance: "clientSupplied"});
}

runTests(true /*prepare*/);
runTests(false /*prepare*/);

session.endSession();
rst.stopSet();
}());
