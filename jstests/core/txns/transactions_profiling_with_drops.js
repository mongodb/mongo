// Tests that locks acquisitions for profiling in a transaction have a 0-second timeout.
// @tags: [uses_transactions]
(function() {
    "use strict";

    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    const dbName = "test";
    const collName = "transactions_profiling_with_drops";
    const otherCollName = "other";
    const adminDB = db.getSiblingDB("admin");
    const testDB = db.getSiblingDB(dbName);
    const otherColl = testDB[otherCollName];
    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb[collName];

    sessionDb.runCommand({dropDatabase: 1, writeConcern: {w: "majority"}});
    assert.commandWorked(sessionColl.insert({_id: "doc"}, {w: "majority"}));
    assert.commandWorked(otherColl.insert({_id: "doc"}, {w: "majority"}));
    assert.commandWorked(sessionDb.runCommand({profile: 1, slowms: 1}));

    jsTest.log("Test read profiling with drops.");

    jsTest.log("Start transaction.");
    session.startTransaction();

    jsTest.log("Run a slow read. Profiling in the transaction should succeed.");
    assert.docEq(
        [{_id: "doc"}],
        sessionColl.find({$where: "sleep(1000); return true;"}).comment("read success").toArray());
    profilerHasSingleMatchingEntryOrThrow(
        {profileDB: testDB, filter: {"command.comment": "read success"}});

    jsTest.log("Start a drop, which will hang.");
    let awaitDrop = startParallelShell(function() {
        db.getSiblingDB("test").runCommand({drop: "other", writeConcern: {w: "majority"}});
    });

    // Wait for the drop to have a pending MODE_X lock on the database.
    assert.soon(
        function() {
            return adminDB
                       .aggregate([
                           {$currentOp: {}},
                           {$match: {"command.drop": otherCollName, waitingForLock: true}}
                       ])
                       .itcount() === 1;
        },
        function() {
            return "Failed to find drop in currentOp output: " +
                tojson(adminDB.aggregate([{$currentOp: {}}]));
        });

    jsTest.log("Run a slow read. Profiling in the transaction should fail.");
    assert.docEq(
        [{_id: "doc"}],
        sessionColl.find({$where: "sleep(1000); return true;"}).comment("read failure").toArray());
    session.commitTransaction();
    awaitDrop();
    profilerHasZeroMatchingEntriesOrThrow(
        {profileDB: testDB, filter: {"command.comment": "read failure"}});

    jsTest.log("Test write profiling with drops.");

    // Recreate the "other" collection so it can be dropped again.
    assert.commandWorked(otherColl.insert({_id: "doc"}, {w: "majority"}));

    jsTest.log("Start transaction.");
    session.startTransaction();

    jsTest.log("Run a slow write. Profiling in the transaction should succeed.");
    assert.commandWorked(sessionColl.update(
        {$where: "sleep(1000); return true;"}, {$inc: {good: 1}}, {collation: {locale: "en"}}));
    profilerHasSingleMatchingEntryOrThrow(
        {profileDB: testDB, filter: {"command.collation": {locale: "en"}}});

    jsTest.log("Start a drop, which will hang.");
    awaitDrop = startParallelShell(function() {
        db.getSiblingDB("test").runCommand({drop: "other", writeConcern: {w: "majority"}});
    });

    // Wait for the drop to have a pending MODE_X lock on the database.
    assert.soon(
        function() {
            return adminDB
                       .aggregate([
                           {$currentOp: {}},
                           {$match: {"command.drop": otherCollName, waitingForLock: true}}
                       ])
                       .itcount() === 1;
        },
        function() {
            return "Failed to find drop in currentOp output: " +
                tojson(adminDB.aggregate([{$currentOp: {}}]));
        });

    jsTest.log("Run a slow write. Profiling in the transaction should fail.");
    assert.commandWorked(sessionColl.update(
        {$where: "sleep(1000); return true;"}, {$inc: {bad: 1}}, {collation: {locale: "fr"}}));
    session.commitTransaction();
    awaitDrop();
    profilerHasZeroMatchingEntriesOrThrow(
        {profileDB: testDB, filter: {"command.collation": {locale: "fr"}}});

    jsTest.log("Both writes should succeed, even if profiling failed.");
    assert.docEq({_id: "doc", good: 1, bad: 1}, sessionColl.findOne());

    session.endSession();
}());
