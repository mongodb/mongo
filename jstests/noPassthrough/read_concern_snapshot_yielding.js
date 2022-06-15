// Test that the read concern level 'snapshot' exhibits the correct yielding behavior. That is,
// operations performed at read concern level snapshot check for interrupt but do not yield locks or
// storage engine resources.
// @tags: [
//   uses_transactions,
// ]
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");  // For waitForCurOpByFailPoint().

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const db = rst.getPrimary().getDB(dbName);
const adminDB = db.getSiblingDB("admin");
const coll = db.coll;
TestData.numDocs = 4;

// Set 'internalQueryExecYieldIterations' to 2 to ensure that commands yield on the second try
// (i.e. after they have established a snapshot but before they have returned any documents).
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 2}));

// Set 'internalQueryExecYieldPeriodMS' to 24 hours to significantly reduce a probability of a
// situation occuring where the execution threads do not receive enough CPU time and commands yield
// on timeout (i.e. yield period expiration) instead of on the second try as expected by setting
// parameter 'internalQueryExecYieldIterations' to 2.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 86400000}));

function assertKillPending(opId) {
    const res =
        adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName(), opid: opId}}])
            .toArray();
    assert.eq(
        res.length,
        1,
        tojson(
            adminDB.aggregate([{$currentOp: {}}, {$match: {ns: coll.getFullName()}}]).toArray()));
    assert(res[0].hasOwnProperty("killPending"), tojson(res));
    assert.eq(true, res[0].killPending, tojson(res));
}

function populateCollection() {
    db.coll.drop({writeConcern: {w: "majority"}});
    for (let i = 0; i < TestData.numDocs; i++) {
        assert.commandWorked(
            db.coll.insert({_id: i, x: 1, location: [0, 0]}, {writeConcern: {w: "majority"}}));
    }

    assert.commandWorked(db.runCommand({
        createIndexes: "coll",
        indexes: [{key: {location: "2d"}, name: "geo_2d"}],
        writeConcern: {w: "majority"}
    }));
}

function testCommand(awaitCommandFn, curOpFilter, testWriteConflict) {
    //
    // Test that the command can be killed.
    //

    TestData.txnNumber++;
    populateCollection();

    // Start a command that hangs before checking for interrupt.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
    let awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);

    // Kill the command, and check that it is set to killPending.
    const curOps = waitForCurOpByFailPoint(
        db, coll.getFullName(), "setInterruptOnlyPlansCheckForInterruptHang", curOpFilter);

    const opId = curOps[0].opid;

    assert.commandWorked(db.killOp(opId));
    assertKillPending(opId);

    // Remove the hang, and check that the command is killed.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
    let exitCode = awaitCommand({checkExitSuccess: false});
    assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

    //
    // Test that the command does not yield locks.
    //

    TestData.txnNumber++;
    populateCollection();

    // Start a command that hangs before checking for interrupt.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
    awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
    waitForCurOpByFailPoint(
        db, coll.getFullName(), "setInterruptOnlyPlansCheckForInterruptHang", curOpFilter);

    // Start a drop. This should block behind the command, since the command does not yield
    // locks.
    let awaitDrop = startParallelShell(function() {
        db.getSiblingDB("test").coll.drop({writeConcern: {w: "majority"}});
    }, rst.ports[0]);

    // Remove the hang. The command should complete successfully.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
    awaitCommand();

    // Now the drop can complete.
    awaitDrop();

    //
    // Test that the command does not read data that is inserted during its execution.
    // 'awaitCommandFn' should fail if it reads the following document:
    //     {_id: <numDocs>, x: 1, new: 1, location: [0, 0]}
    //

    TestData.txnNumber++;
    populateCollection();

    // Start a command that hangs before checking for interrupt.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
    awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
    waitForCurOpByFailPoint(
        db, coll.getFullName(), "setInterruptOnlyPlansCheckForInterruptHang", curOpFilter);

    // Insert data that should not be read by the command.
    assert.commandWorked(db.coll.insert({_id: TestData.numDocs, x: 1, new: 1, location: [0, 0]},
                                        {writeConcern: {w: "majority"}}));

    // Remove the hang. The command should complete successfully.
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
    awaitCommand();

    //
    // Test that the command fails if a write conflict occurs. 'awaitCommandFn' should write to
    // the following document: {_id: <numDocs>, x: 1, new: 1, location: [0, 0]}
    //

    if (testWriteConflict) {
        TestData.txnNumber++;
        populateCollection();

        // Insert the document that the command will write to.
        assert.commandWorked(db.coll.insert({_id: TestData.numDocs, x: 1, new: 1, location: [0, 0]},
                                            {writeConcern: {w: "majority"}}));

        // Start a command that hangs before checking for interrupt.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
        awaitCommand = startParallelShell(awaitCommandFn, rst.ports[0]);
        waitForCurOpByFailPoint(
            db, coll.getFullName(), "setInterruptOnlyPlansCheckForInterruptHang", curOpFilter);

        // Update the document that the command will write to.
        assert.commandWorked(db.coll.update(
            {_id: TestData.numDocs}, {$set: {conflict: true}}, {writeConcern: {w: "majority"}}));

        // Remove the hang. The command should fail.
        assert.commandWorked(db.adminCommand(
            {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
        exitCode = awaitCommand({checkExitSuccess: false});
        assert.neq(0, exitCode, "Expected shell to exit with failure due to WriteConflict");
    }
}

// Test find.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(sessionDb.runCommand({find: "coll", filter: {x: 1}}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(res.cursor.firstBatch.length, TestData.numDocs, tojson(res));
}, {"command.filter": {x: 1}});

// Test getMore on a find established cursor.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
    const initialFindBatchSize = 2;
    const cursorId = assert
                         .commandWorked(sessionDb.runCommand(
                             {find: "coll", filter: {x: 1}, batchSize: initialFindBatchSize}))
                         .cursor.id;
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
    const res = assert.commandWorked(sessionDb.runCommand(
        {getMore: NumberLong(cursorId), collection: "coll", batchSize: TestData.numDocs}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(res.cursor.nextBatch.length, TestData.numDocs - initialFindBatchSize, tojson(res));
}, {"cursor.originatingCommand.filter": {x: 1}});

// Test aggregate.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(
        sessionDb.runCommand({aggregate: "coll", pipeline: [{$match: {x: 1}}], cursor: {}}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(res.cursor.firstBatch.length, TestData.numDocs, tojson(res));
}, {"command.pipeline": [{$match: {x: 1}}]});

// Test getMore with an initial find batchSize of 0. Interrupt behavior of a getMore is not expected
// to change with a change of batchSize in the originating command.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "off"}));
    const initialFindBatchSize = 0;
    const cursorId = assert
                         .commandWorked(sessionDb.runCommand(
                             {find: "coll", filter: {x: 1}, batchSize: initialFindBatchSize}))
                         .cursor.id;
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "setInterruptOnlyPlansCheckForInterruptHang", mode: "alwaysOn"}));
    const res = assert.commandWorked(
        sessionDb.runCommand({getMore: NumberLong(cursorId), collection: "coll"}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert.eq(res.cursor.nextBatch.length, TestData.numDocs - initialFindBatchSize, tojson(res));
}, {"cursor.originatingCommand.filter": {x: 1}});

// Test distinct.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(sessionDb.runCommand({distinct: "coll", key: "_id"}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert(res.hasOwnProperty("values"));
    assert.eq(res.values.length, 4, tojson(res));
}, {"command.distinct": "coll"});

// Test update.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(sessionDb.runCommand({
        update: "coll",
        updates: [{q: {}, u: {$set: {updated: true}}}, {q: {new: 1}, u: {$set: {updated: true}}}]
    }));
    assert.commandWorked(session.commitTransaction_forTesting());
    // Only update one existing doc committed before the transaction.
    assert.eq(res.n, 1, tojson(res));
    assert.eq(res.nModified, 1, tojson(res));
}, {op: "update"}, true);

// Test delete.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(sessionDb.runCommand(
        {delete: "coll", deletes: [{q: {}, limit: 1}, {q: {new: 1}, limit: 1}]}));
    assert.commandWorked(session.commitTransaction_forTesting());
    // Only remove one existing doc committed before the transaction.
    assert.eq(res.n, 1, tojson(res));
}, {op: "remove"}, true);

// Test findAndModify.
testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(sessionDb.runCommand(
        {findAndModify: "coll", query: {new: 1}, update: {$set: {findAndModify: 1}}}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert(res.hasOwnProperty("lastErrorObject"));
    assert.eq(res.lastErrorObject.n, 0, tojson(res));
    assert.eq(res.lastErrorObject.updatedExisting, false, tojson(res));
}, {"command.findAndModify": "coll"}, true);

testCommand(function() {
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase("test");
    session.startTransaction({readConcern: {level: "snapshot"}});
    const res = assert.commandWorked(sessionDb.runCommand(
        {findAndModify: "coll", query: {new: 1}, update: {$set: {findAndModify: 1}}}));
    assert.commandWorked(session.commitTransaction_forTesting());
    assert(res.hasOwnProperty("lastErrorObject"));
    assert.eq(res.lastErrorObject.n, 0, tojson(res));
    assert.eq(res.lastErrorObject.updatedExisting, false, tojson(res));
}, {"command.findAndModify": "coll"}, true);

rst.stopSet();
}());
