/**
 * Ensures that performing a write in a prepared transaction, followed by a write outside of a
 * transaction, it is possible to specify '$_internalReadAtClusterTime' as the timestamp of
 * the second write for 'find' and 'dbHash'. The commands should block until the prepared
 * transaction is committed or aborted.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/parallelTester.js");

const runDBHashFn = (host, dbName, clusterTime) => {
    const conn = new Mongo(host);
    const db = conn.getDB(dbName);

    conn.setSlaveOk();
    let firstHash = assert.commandWorked(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: eval(clusterTime),
    }));

    // This code will execute once the prepared transaction is committed as the call above will
    // be blocked until an abort or commit happens. Ensure that running dbHash here yields the
    // same result as above.
    let secondHash = assert.commandWorked(db.runCommand({dbHash: 1}));

    assert.eq(firstHash.collections, secondHash.collections);
    assert.eq(firstHash.md5, secondHash.md5);

    return firstHash;
};

const runFindFn = (host, dbName, collName, clusterTime) => {
    const conn = new Mongo(host);
    const db = conn.getDB(dbName);

    conn.setSlaveOk();
    assert.commandWorked(db.getSiblingDB(dbName).runCommand({
        find: collName,
        $_internalReadAtClusterTime: eval(clusterTime),
    }));
};

const assertOpHasPrepareConflict = (db, commandName) => {
    assert.soon(
        () => {
            const ops = db.currentOp({
                              "command.$_internalReadAtClusterTime": {$exists: true},
                              ["command." + commandName]: {$exists: true},
                          }).inprog;

            if (ops.length === 1) {
                return ops[0].prepareReadConflicts > 0;
            }

            return false;
        },
        () => `Failed to find '${commandName}' command in the ${db.getMongo().host} currentOp()` +
            ` output: ${tojson(db.currentOp())}`);
};

const rst = new ReplSetTest({nodes: 2});
rst.startSet();

const replSetConfig = rst.getReplSetConfig();
replSetConfig.members[1].priority = 0;
rst.initiate(replSetConfig);

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const dbName = "prepare_transaction_read_at_cluster_time";
const collName = "testColl";

const testDB = primary.getDB(dbName);
const testDBSecondary = secondary.getDB(dbName);

testDB.createCollection(collName);
assert.commandWorked(testDB.getCollection(collName).insert({x: 0}));

const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB[collName];

// Perform a write inside of a prepared transaction.
session.startTransaction();
assert.commandWorked(sessionColl.insert({x: 1}));
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

// Perform a write outside of a prepared transaction. We wait for the write to have replication
// to the secondary because we're going to read from it at the returned operationTime.
assert.commandWorked(testDB.getCollection(collName).insert({x: 2}, {writeConcern: {w: 2}}));

// It should be possible to specify '$_internalReadAtClusterTime' as the timestamp of the
// second write without an error for dbHash and find.
let clusterTime = testDB.getSession().getOperationTime();

// Run dbHash and find while the prepared transaction has not commit or aborted yet.
// These should block until the prepared transaction commits or aborts if we specify
// $_internalReadAtClusterTime to be the timestamp of the second write we did, outside of the
// transaction.
const dbHashPrimaryThread = new Thread(runDBHashFn, primary.host, dbName, tojson(clusterTime));
const dbHashSecondaryThread = new Thread(runDBHashFn, secondary.host, dbName, tojson(clusterTime));

dbHashPrimaryThread.start();
dbHashSecondaryThread.start();

assertOpHasPrepareConflict(testDB, "dbHash");
assertOpHasPrepareConflict(testDBSecondary, "dbHash");

// Run 'find' with '$_internalReadAtClusterTime' specified.
const findPrimaryThread =
    new Thread(runFindFn, primary.host, dbName, collName, tojson(clusterTime));
const findSecondaryThread =
    new Thread(runFindFn, secondary.host, dbName, collName, tojson(clusterTime));

findPrimaryThread.start();
findSecondaryThread.start();

assertOpHasPrepareConflict(testDB, "find");
assertOpHasPrepareConflict(testDBSecondary, "find");

// Run a series of DDL operations which shouldn't block before committing the prepared
// transaction.
const otherDbName = "prepare_transaction_read_at_cluster_time_secondary_other";
const otherTestDB = primary.getDB(otherDbName);

assert.commandWorked(otherTestDB.runCommand({create: collName, writeConcern: {w: 2}}));
assert.commandWorked(
    otherTestDB.runCommand({collMod: collName, validator: {v: 1}, writeConcern: {w: 2}}));
assert.commandWorked(otherTestDB.runCommand(
    {createIndexes: collName, indexes: [{key: {x: 1}, name: 'x_1'}], writeConcern: {w: 2}}));
assert.commandWorked(
    otherTestDB.runCommand({dropIndexes: collName, index: 'x_1', writeConcern: {w: 2}}));

// Committing or aborting the transaction should unblock the parallel tasks.
PrepareHelpers.commitTransaction(session, prepareTimestamp);
session.endSession();

dbHashPrimaryThread.join();
dbHashSecondaryThread.join();

// Ensure the dbHashes across the replica set match.
const primaryDBHash = dbHashPrimaryThread.returnData();
const secondaryDBHash = dbHashSecondaryThread.returnData();

assert.eq(primaryDBHash.collections, secondaryDBHash.collections);
assert.eq(primaryDBHash.md5, secondaryDBHash.md5);

findPrimaryThread.join();
findSecondaryThread.join();

rst.stopSet();
}());
