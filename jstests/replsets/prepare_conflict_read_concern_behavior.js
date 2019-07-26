/**
 * Test calling reads with various read concerns on a prepared transaction. Snapshot, linearizable
 * and afterClusterTime reads are the only reads that should block on a prepared transaction. Reads
 * that happen as part of a write should also block on a prepared transaction.
 *
 * Also test that dbHash and mapReduce, which acquire collection S locks for reads, do not block on
 * a prepared transaction on secondaries. Otherwise, it would cause deadlocks when the prepared
 * transaction reacquires locks (since locks were yielded on secondaries) at commit time. This test
 * makes sure dbHash and mapReduce do not accept a non local read concern or afterClusterTime and so
 * it is safe for the two commands to ignore prepare conflicts for reads. This test also makes sure
 * mapReduce that does writes is not allowed to run on secondaries.
 *
 * Also test that validate, which acquires collection X lock during its execution, does not block on
 * a prepared transaction on secondaries. Otherwise, it would cause deadlocks when the prepared
 * transaction reacquires locks (since locks were yielded on secondaries) at commit time. This test
 * makes sure the validate command does not accept a non local read concern or afterClusterTime and
 * that it is therefore safe to ignore prepare conflicts during its execution.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
load("jstests/core/txns/libs/prepare_helpers.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const conn = replTest.getPrimary();

const failureTimeout = 1 * 1000;       // 1 second.
const successTimeout = 5 * 60 * 1000;  // 5 minutes.
const dbName = "test";
const collName = "prepare_conflict_read_concern_behavior";
const collName2 = "prepare_conflict_read_concern_behavior2";
const testDB = conn.getDB(dbName);
const testColl = testDB.getCollection(collName);
const testColl2 = testDB.getCollection(collName2);

const secondary = replTest.getSecondary();
const secondaryTestDB = secondary.getDB(dbName);

// Turn off timestamp reaping so that clusterTimeBeforePrepare doesn't get too old.
assert.commandWorked(testDB.adminCommand({
    configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
    mode: "alwaysOn",
}));

function runTest() {
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    testDB.runCommand({drop: collName2, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName2, writeConcern: {w: "majority"}}));

    const session = conn.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);

    const read = function(read_concern, timeout, db, coll, num_expected) {
        let res = db.runCommand({
            find: coll,
            filter: {in_prepared_txn: false},
            readConcern: read_concern,
            maxTimeMS: timeout,
        });

        if (num_expected) {
            assert(res.cursor, tojson(res));
            assert.eq(res.cursor.firstBatch.length, num_expected, tojson(res));
        }
        return res;
    };

    const dbHash = function(read_concern, db, timeout = successTimeout) {
        let res = db.runCommand({
            dbHash: 1,
            readConcern: read_concern,
            maxTimeMS: timeout,
        });

        return res;
    };

    const mapReduce = function(
        read_concern, db, outOptions = {inline: 1}, timeout = successTimeout) {
        let map = function() {
            emit(this.a, this.a);
        };
        let reduce = function(key, vals) {
            return 1;
        };
        let res = db.runCommand({
            mapReduce: collName,
            map: map,
            reduce: reduce,
            out: outOptions,
            readConcern: read_concern,
            maxTimeMS: timeout,
        });
        return res;
    };

    const validate = function(read_concern, db, timeout = successTimeout) {
        let res = db.runCommand({
            validate: collName,
            readConcern: read_concern,
            maxTimeMS: timeout,
        });

        return res;
    };

    assert.commandWorked(
        testColl.insert({_id: 1, in_prepared_txn: false}, {writeConcern: {w: "majority"}}));
    assert.commandWorked(testColl.insert({_id: 2, in_prepared_txn: false}));
    assert.commandWorked(testColl2.insert({_id: 1, in_prepared_txn: false}));

    session.startTransaction();
    const clusterTimeBeforePrepare =
        assert.commandWorked(sessionColl.runCommand("insert", {documents: [{_id: 3}]}))
            .operationTime;
    assert.commandWorked(sessionColl.update({_id: 2}, {_id: 2, in_prepared_txn: true}));
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    const clusterTimeAfterPrepare =
        assert
            .commandWorked(testColl.runCommand(
                "insert",
                {documents: [{_id: 4, in_prepared_txn: false}], writeConcern: {w: "majority"}}))
            .operationTime;

    jsTestLog("prepareTimestamp: " + prepareTimestamp + " clusterTimeBeforePrepare: " +
              clusterTimeBeforePrepare + " clusterTimeAfterPrepare: " + clusterTimeAfterPrepare);

    assert.gt(prepareTimestamp, clusterTimeBeforePrepare);
    assert.gt(clusterTimeAfterPrepare, prepareTimestamp);

    jsTestLog("Test read with read concern 'majority' doesn't block on a prepared transaction.");
    assert.commandWorked(read({level: 'majority'}, successTimeout, testDB, collName, 3));

    jsTestLog("Test read with read concern 'local' doesn't block on a prepared transaction.");
    assert.commandWorked(read({level: 'local'}, successTimeout, testDB, collName, 3));

    jsTestLog("Test read with read concern 'available' doesn't block on a prepared transaction.");
    assert.commandWorked(read({level: 'available'}, successTimeout, testDB, collName, 3));

    jsTestLog("Test read with read concern 'linearizable' blocks on a prepared transaction.");
    assert.commandFailedWithCode(read({level: 'linearizable'}, failureTimeout, testDB, collName),
                                 ErrorCodes.MaxTimeMSExpired);

    // TODO SERVER-36953: uncomment this test
    // jsTestLog("Test afterClusterTime read before prepareTimestamp doesn't block on a " +
    //           "prepared transaction.");
    // assert.commandWorked(read({level: 'local', afterClusterTime: clusterTimeBeforePrepare},
    //                           successTimeout,
    //                           testDB,
    //                           collName,
    //                           2));

    jsTestLog("Test afterClusterTime read after prepareTimestamp blocks on a prepared " +
              "transaction.");
    assert.commandFailedWithCode(read({level: 'local', afterClusterTime: clusterTimeAfterPrepare},
                                      failureTimeout,
                                      testDB,
                                      collName),
                                 ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Test read with afterClusterTime after prepareTimestamp on non-prepared " +
              "documents doesn't block on a prepared transaction.");
    assert.commandWorked(read({level: 'local', afterClusterTime: clusterTimeAfterPrepare},
                              successTimeout,
                              testDB,
                              collName2,
                              1));

    // dbHash does not accept a non local read concern or afterClusterTime and it also sets
    // ignore_prepare=true during its execution. Therefore, dbHash should never get prepare
    // conflicts on secondaries. dbHash acquires collection S lock for reads and it will be
    // blocked by a prepared transaction that writes to the same collection if it is run on
    // primaries.
    jsTestLog("Test dbHash doesn't support afterClusterTime read.");
    assert.commandFailedWithCode(
        dbHash({level: 'local', afterClusterTime: clusterTimeAfterPrepare}, secondaryTestDB),
        ErrorCodes.InvalidOptions);

    jsTestLog("Test dbHash doesn't support read concern other than local.");
    assert.commandWorked(dbHash({level: 'local'}, secondaryTestDB));
    assert.commandFailedWithCode(dbHash({level: 'available'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(dbHash({level: 'majority'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(dbHash({level: 'snapshot'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(dbHash({level: 'linearizable'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Test dbHash on secondary doesn't block on a prepared transaction.");
    assert.commandWorked(dbHash({}, secondaryTestDB));
    jsTestLog("Test dbHash on primary blocks on collection S lock which conflicts with " +
              "a prepared transaction.");
    assert.commandFailedWithCode(dbHash({}, testDB, failureTimeout), ErrorCodes.MaxTimeMSExpired);

    // mapReduce does not accept a non local read concern or afterClusterTime and it also sets
    // ignore_prepare=true during its read phase. As mapReduce that writes is not allowed to run
    // on secondaries, mapReduce should never get prepare conflicts on secondaries. mapReduce
    // acquires collection S lock for reads and it will be blocked by a prepared transaction
    // that writes to the same collection if it is run on primaries.
    jsTestLog("Test mapReduce doesn't support afterClusterTime read.");
    assert.commandFailedWithCode(
        mapReduce({level: 'local', afterClusterTime: clusterTimeAfterPrepare}, secondaryTestDB),
        ErrorCodes.InvalidOptions);

    jsTestLog("Test mapReduce doesn't support read concern other than local.");
    assert.commandWorked(mapReduce({level: 'local'}, secondaryTestDB));
    assert.commandFailedWithCode(mapReduce({level: 'available'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(mapReduce({level: 'majority'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(mapReduce({level: 'snapshot'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(mapReduce({level: 'linearizable'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Test mapReduce that writes is not allowed to run on secondaries.");
    // It currently returns ErrorCodes.PrimarySteppedDown in this case.
    assert.commandFailedWithCode(mapReduce({}, secondaryTestDB, "outColl"),
                                 [ErrorCodes.InvalidOptions, ErrorCodes.PrimarySteppedDown]);

    jsTestLog("Test mapReduce on secondary doesn't block on a prepared transaction.");
    assert.commandWorked(mapReduce({}, secondaryTestDB));

    jsTestLog("Test mapReduce on primary blocks on collection S lock which conflicts with " +
              "a prepared transaction.");
    assert.commandFailedWithCode(mapReduce({}, testDB, {inline: 1}, failureTimeout),
                                 ErrorCodes.MaxTimeMSExpired);

    // validate does not accept a non local read concern or afterClusterTime and it also sets
    // ignore_prepare=true during its execution. Therefore, validate should never get prepare
    // conflicts on secondaries. validate acquires collection X lock during its execution and it
    // will be blocked by a prepared transaction that writes to the same collection if it is run
    // on primaries.
    jsTestLog("Test validate doesn't support afterClusterTime read.");
    assert.commandFailedWithCode(
        validate({level: 'local', afterClusterTime: clusterTimeAfterPrepare}, secondaryTestDB),
        ErrorCodes.InvalidOptions);
    jsTestLog("Test validate doesn't support read concern other than local.");
    assert.commandWorked(validate({level: 'local'}, secondaryTestDB));
    assert.commandFailedWithCode(validate({level: 'available'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(validate({level: 'majority'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(validate({level: 'snapshot'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(validate({level: 'linearizable'}, secondaryTestDB),
                                 ErrorCodes.InvalidOptions);

    jsTestLog("Test validate on secondary doesn't block on a prepared transaction.");
    assert.commandWorked(validate({}, secondaryTestDB));
    jsTestLog("Test validate on primary blocks on collection X lock which conflicts with " +
              "a prepared transaction.");
    assert.commandFailedWithCode(validate({}, testDB, failureTimeout), ErrorCodes.MaxTimeMSExpired);

    jsTestLog("Test read from an update blocks on a prepared transaction.");
    assert.commandFailedWithCode(testDB.runCommand({
        update: collName,
        updates: [{q: {_id: 2}, u: {_id: 2, in_prepared_txn: false, a: 1}}],
        maxTimeMS: failureTimeout,
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Create a second session and start a new transaction to test snapshot reads.
    const session2 = conn.startSession({causalConsistency: false});
    const sessionDB2 = session2.getDatabase(dbName);
    const sessionColl2 = sessionDB2.getCollection(collName);
    // This makes future reads in the transaction use a read timestamp after the
    // prepareTimestamp.
    session2.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeAfterPrepare}});

    jsTestLog("Test read with read concern 'snapshot' and a read timestamp after " +
              "prepareTimestamp on non-prepared documents doesn't block on a prepared " +
              "transaction.");
    assert.commandWorked(read({}, successTimeout, sessionDB2, collName2, 1));

    jsTestLog("Test read with read concern 'snapshot' and a read timestamp after " +
              "prepareTimestamp blocks on a prepared transaction.");
    assert.commandFailedWithCode(read({}, failureTimeout, sessionDB2, collName),
                                 ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(session2.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test read with read concern 'snapshot' and atClusterTime before " +
              "prepareTimestamp doesn't block on a prepared transaction.");
    session2.startTransaction(
        {readConcern: {level: "snapshot", atClusterTime: clusterTimeBeforePrepare}});
    assert.commandWorked(read({}, successTimeout, sessionDB2, collName, 2));
    assert.commandWorked(session2.abortTransaction_forTesting());

    jsTestLog("Test read from a transaction with read concern 'majority' blocks on a prepared" +
              " transaction.");
    session2.startTransaction({readConcern: {level: "majority"}});
    assert.commandFailedWithCode(read({}, failureTimeout, sessionDB2, collName),
                                 ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(session2.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test read from a transaction with read concern 'local' blocks on a prepared " +
              "transaction.");
    session2.startTransaction({readConcern: {level: "local"}});
    assert.commandFailedWithCode(read({}, failureTimeout, sessionDB2, collName),
                                 ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(session2.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test read from a transaction with no read concern specified blocks on a " +
              "prepared transaction.");
    session2.startTransaction();
    assert.commandFailedWithCode(read({}, failureTimeout, sessionDB2, collName),
                                 ErrorCodes.MaxTimeMSExpired);
    assert.commandFailedWithCode(session2.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
    session2.endSession();

    assert.commandWorked(session.abortTransaction_forTesting());
    session.endSession();
}

try {
    runTest();
} finally {
    // Turn this failpoint off so that it doesn't impact other tests in the suite.
    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
        mode: "off",
    }));
}

replTest.stopSet();
}());
