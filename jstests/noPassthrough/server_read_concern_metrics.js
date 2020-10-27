// Tests readConcern level metrics in the serverStatus output.
// @tags: [
//   requires_majority_read_concern,
//   requires_persistence,
//   uses_transactions,
// ]
(function() {
"use strict";

// Verifies that the server status response has the fields that we expect.
function verifyServerStatusFields(serverStatusResponse) {
    assert(serverStatusResponse.hasOwnProperty("readConcernCounters"),
           "Missing 'readConcernCounters' from serverStatus\n" + tojson(serverStatusResponse));

    assert(serverStatusResponse.readConcernCounters.hasOwnProperty("nonTransactionOps"),
           "Missing 'nonTransactionOps' from 'readConcernCounters'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.hasOwnProperty("none"),
           "Missing 'none' from 'readConcernCounters.nonTransactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.hasOwnProperty("local"),
           "Missing 'local' from 'readConcernCounters.nonTransactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.hasOwnProperty("available"),
           "Missing 'available' from 'readConcernCounters.nonTransactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.hasOwnProperty("majority"),
           "Missing 'majority' from 'readConcernCounters.nonTransactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.hasOwnProperty("snapshot"),
           "Missing 'snapshot' from 'readConcernCounters.nonTransactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.snapshot.hasOwnProperty(
               "withClusterTime"),
           "Missing 'withClusterTime' from 'readConcernCounters.nonTransactionOps.snapshot'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.nonTransactionOps.snapshot.hasOwnProperty(
               "withoutClusterTime"),
           "Missing 'withoutClusterTime' from 'readConcernCounters.nonTransactionOps.snapshot'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(
        serverStatusResponse.readConcernCounters.nonTransactionOps.hasOwnProperty("linearizable"),
        "Missing 'linearizable' from 'readConcernCounters.nonTransactionOps'\n" +
            tojson(serverStatusResponse.readConcernCounters));

    assert(serverStatusResponse.readConcernCounters.hasOwnProperty("transactionOps"),
           "Missing 'transactionOps' from 'readConcernCounters'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.transactionOps.hasOwnProperty("none"),
           "Missing 'none' from 'readConcernCounters.transactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.transactionOps.hasOwnProperty("local"),
           "Missing 'local' from 'readConcernCounters.transactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.transactionOps.hasOwnProperty("majority"),
           "Missing 'majority' from 'readConcernCounters.transactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.transactionOps.hasOwnProperty("snapshot"),
           "Missing 'snapshot' from 'readConcernCounters.transactionOps'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.transactionOps.snapshot.hasOwnProperty(
               "withClusterTime"),
           "Missing 'withClusterTime' from 'readConcernCounters.transactionOps.snapshot'\n" +
               tojson(serverStatusResponse.readConcernCounters));
    assert(serverStatusResponse.readConcernCounters.transactionOps.snapshot.hasOwnProperty(
               "withoutClusterTime"),
           "Missing 'withoutClusterTime' from 'readConcernCounters.transactionOps.snapshot'\n" +
               tojson(serverStatusResponse.readConcernCounters));
}

// Verifies that the given value of the server status response is incremented in the way
// we expect.
function verifyServerStatusChange(initialStatus,
                                  newStatus,
                                  field,
                                  expectedIncrement,
                                  {isTransaction = false, atClusterTime = false} = {}) {
    verifyServerStatusFields(newStatus);
    let initialCounters = initialStatus.readConcernCounters;
    let newCounters = newStatus.readConcernCounters;
    let initialOps, newOps;
    let fieldPath = "serverStatus.readConcernCounters";
    if (isTransaction) {
        initialOps = initialCounters.transactionOps;
        newOps = newCounters.transactionOps;
        fieldPath = fieldPath + ".transactionOps";
    } else {
        initialOps = initialCounters.nonTransactionOps;
        newOps = newCounters.nonTransactionOps;
        fieldPath = fieldPath + ".nonTransactionOps";
    }

    if (field === "snapshot") {
        initialOps = initialOps.snapshot;
        newOps = newOps.snapshot;
        fieldPath = fieldPath + ".snapshot";
        if (atClusterTime) {
            field = "withClusterTime";
        } else {
            field = "withoutClusterTime";
        }
    }

    fieldPath = fieldPath + "." + field;
    assert.eq(initialOps[field] + expectedIncrement,
              newOps[field],
              "expected " + fieldPath + " to increase by " + expectedIncrement +
                  ", initialStats: " + tojson(initialCounters) +
                  ", newStats: " + tojson(newCounters));

    // Update the value of the field to the new value so we can compare the rest of the fields using
    // assert.docEq.
    initialOps[field] = newOps[field];

    assert.docEq(initialCounters,
                 newCounters,
                 "Expected docEq after updating " + fieldPath + ", initialCounters: " +
                     tojson(initialCounters + ", newCounters: " + tojson(newCounters)));
}

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const dbName = "test";
const collName = "server_read_concern_metrics";
const testDB = primary.getDB(dbName);
const testColl = testDB[collName];
const sessionOptions = {
    causalConsistency: false
};
const session = testDB.getMongo().startSession(sessionOptions);
const sessionDb = session.getDatabase(dbName);
const sessionColl = sessionDb[collName];

testDB.runCommand({drop: collName});
assert.commandWorked(testDB.createCollection(collName));
assert.commandWorked(testColl.insert({_id: 0}));

// Run an initial transaction to get config.transactions state into memory.
session.startTransaction();
assert.eq(sessionColl.find().itcount(), 1);
assert.commandWorked(session.abortTransaction_forTesting());

function getServerStatus(conn) {
    // Don't return defaultRWConcern because it may trigger a refresh of the read write concern
    // defaults, which unexpectedly increases the opReadConcernCounters.
    return assert.commandWorked(conn.adminCommand({serverStatus: 1, defaultRWConcern: false}));
}

// Get initial serverStatus.
let serverStatus = getServerStatus(testDB);
verifyServerStatusFields(serverStatus);

let newStatus;

// Run a legacy query.
primary.forceReadMode("legacy");
assert.eq(testColl.find().itcount(), 1);
newStatus = getServerStatus(testDB);
verifyServerStatusChange(serverStatus, newStatus, "none", 1);
primary.forceReadMode("commands");
serverStatus = newStatus;

// Run a command without a readConcern.
assert.eq(testColl.find().itcount(), 1);
newStatus = getServerStatus(testDB);
verifyServerStatusChange(serverStatus, newStatus, "none", 1);
serverStatus = newStatus;

// Non-transaction reads.
for (let level of ["none", "local", "available", "snapshot", "majority", "linearizable"]) {
    jsTestLog("Testing non-transaction reads with readConcern " + level);
    let readConcern = {};
    if (level !== "none") {
        readConcern = {level: level};
    }

    assert.commandWorked(testDB.runCommand({find: collName, readConcern: readConcern}));
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 1);
    serverStatus = newStatus;

    assert.commandWorked(testDB.runCommand(
        {aggregate: collName, pipeline: [], cursor: {}, readConcern: readConcern}));
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 1);
    serverStatus = newStatus;

    assert.commandWorked(
        testDB.runCommand({distinct: collName, key: "_id", readConcern: readConcern}));
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 1);
    serverStatus = newStatus;

    if (level !== "snapshot") {
        assert.commandWorked(testDB.runCommand({count: collName, readConcern: readConcern}));
        newStatus = getServerStatus(testDB);
        verifyServerStatusChange(serverStatus, newStatus, level, 1);
        serverStatus = newStatus;
    }

    if (level in ["none", "local", "available"]) {
        // mapReduce only support local and available.
        assert.commandWorked(testDB.runCommand({
            mapReduce: collName,
            map: function() {
                emit(this.a, this.a);
            },
            reduce: function(key, vals) {
                return 1;
            },
            out: {inline: 1},
            readConcern: readConcern
        }));
        newStatus = getServerStatus(testDB);
        verifyServerStatusChange(serverStatus, newStatus, level, 1);
        serverStatus = newStatus;
    }

    // getMore does not count toward readConcern metrics. getMore inherits the readConcern of the
    // originating command.
    let res = assert.commandWorked(
        testDB.runCommand({find: collName, batchSize: 0, readConcern: readConcern}));
    serverStatus = getServerStatus(testDB);
    assert.commandWorked(testDB.runCommand({getMore: res.cursor.id, collection: collName}));
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 0);
}

// Test non-transaction snapshot with atClusterTime.
let insertTimestamp =
    assert.commandWorked(testDB.runCommand({insert: "atClusterTime", documents: [{_id: 0}]}))
        .operationTime;
jsTestLog("Testing non-transaction reads with atClusterTime");
serverStatus = getServerStatus(testDB);
assert.commandWorked(testDB.runCommand(
    {find: "atClusterTime", readConcern: {level: "snapshot", atClusterTime: insertTimestamp}}));
newStatus = getServerStatus(testDB);
verifyServerStatusChange(serverStatus, newStatus, "snapshot", 1, {atClusterTime: true});
serverStatus = newStatus;

// Transaction reads.
for (let level of ["none", "local", "snapshot", "majority"]) {
    jsTestLog("Testing transaction reads with readConcern " + level);
    if (level === "none") {
        session.startTransaction();
    } else {
        session.startTransaction({readConcern: {level: level}});
    }
    assert.eq(sessionColl.find().itcount(), 1);
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 1, {isTransaction: true});
    serverStatus = newStatus;

    // Run a second find in the same transaction. It will inherit the readConcern from the
    // transaction.
    assert.eq(sessionColl.find().itcount(), 1);
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 1, {isTransaction: true});
    serverStatus = newStatus;

    // Run an insert in the same transaction. This should not count toward the readConcern metrics.
    assert.commandWorked(
        sessionDb.runCommand({insert: "transaction", documents: [{level: level}]}));
    newStatus = getServerStatus(testDB);
    verifyServerStatusChange(serverStatus, newStatus, level, 0, {isTransaction: true});
    assert.commandWorked(session.abortTransaction_forTesting());
    serverStatus = newStatus;
}

// Test transaction snapshot with atClusterTime.
insertTimestamp =
    assert.commandWorked(testDB.runCommand({insert: "atClusterTime", documents: [{_id: 1}]}))
        .operationTime;
jsTestLog("Testing transaction reads with atClusterTime");
session.startTransaction({readConcern: {level: "snapshot", atClusterTime: insertTimestamp}});
serverStatus = getServerStatus(testDB);

assert.eq(sessionColl.find().itcount(), 1);
newStatus = getServerStatus(testDB);
verifyServerStatusChange(
    serverStatus, newStatus, "snapshot", 1, {isTransaction: true, atClusterTime: true});
serverStatus = newStatus;

// Perform another read in the same transaction.
assert.eq(sessionColl.find().itcount(), 1);
newStatus = getServerStatus(testDB);
verifyServerStatusChange(
    serverStatus, newStatus, "snapshot", 1, {isTransaction: true, atClusterTime: true});
assert.commandWorked(session.abortTransaction_forTesting());

session.endSession();
rst.stopSet();
}());
