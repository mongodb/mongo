// Test profiling for commands in multi-document transactions.
// @tags: [uses_transactions]
(function() {
    "use strict";
    load("jstests/libs/profiler.js");  // For getLatestProfilerEntry.

    const dbName = "test";
    const collName = "transactions_profiling";
    const testDB = db.getSiblingDB(dbName);
    testDB[collName].drop();

    testDB.setProfilingLevel(2);

    const sessionOptions = {causalConsistency: false};
    let session = testDB.getMongo().startSession(sessionOptions);
    let sessionDB = session.getDatabase(dbName);
    let sessionColl = sessionDB[collName];

    assert.commandWorked(sessionColl.insert({_id: "findAndModify-doc"}));
    assert.commandWorked(sessionColl.insert({_id: "delete-doc"}));
    assert.commandWorked(sessionColl.insert({_id: "multi-delete-doc-1"}));
    assert.commandWorked(sessionColl.insert({_id: "multi-delete-doc-2"}));
    assert.commandWorked(sessionColl.insert({_id: "multi-delete-doc-3"}));
    assert.commandWorked(sessionColl.insert({_id: "multi-delete-doc-4"}));
    assert.commandWorked(sessionColl.insert({_id: "read-doc"}));
    assert.commandWorked(sessionColl.insert({_id: "update-doc"}));
    assert.commandWorked(sessionColl.insert({_id: "multi-update-doc-1"}));
    assert.commandWorked(sessionColl.insert({_id: "multi-update-doc-2"}));
    assert.commandWorked(testDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {haystack: "geoHaystack", a: 1}, name: "haystack_geo", bucketSize: 1}],
        writeConcern: {w: "majority"}
    }));

    jsTestLog("Test commands that can use shell helpers.");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    jsTestLog("Test aggregate.");
    assert.eq(1, sessionColl.aggregate([{$match: {_id: "read-doc"}}]).itcount());
    let profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.command.aggregate, sessionColl.getName(), tojson(profileObj));
    assert.eq(profileObj.nreturned, 1, tojson(profileObj));

    jsTestLog("Test delete.");
    assert.commandWorked(sessionColl.deleteOne({_id: "delete-doc"}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "remove", tojson(profileObj));
    assert.eq(profileObj.ndeleted, 1, tojson(profileObj));

    jsTestLog("Test multi delete.");
    assert.commandWorked(
        sessionColl.deleteMany({_id: {$in: ["multi-delete-doc-1", "multi-delete-doc-2"]}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "remove", tojson(profileObj));
    assert.eq(profileObj.ndeleted, 2, tojson(profileObj));

    jsTestLog("Test batch delete.");
    assert.commandWorked(sessionDB.runCommand({
        delete: collName,
        deletes: [
            {q: {_id: "multi-delete-doc-3"}, limit: 1},
            {q: {_id: "multi-delete-doc-4"}, limit: 1}
        ]
    }));
    // We see the profile entry from the second delete.
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "remove", tojson(profileObj));
    assert.eq(profileObj.ndeleted, 1, tojson(profileObj));

    jsTestLog("Test distinct.");
    assert.eq(["read-doc"], sessionColl.distinct("_id", {_id: "read-doc"}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.command.distinct, sessionColl.getName(), tojson(profileObj));

    jsTestLog("Test find.");
    assert.eq(1, sessionColl.find({_id: "read-doc"}).itcount());
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "query", tojson(profileObj));
    assert.eq(profileObj.nreturned, 1, tojson(profileObj));

    jsTestLog("Test findAndModify.");
    assert.eq({_id: "findAndModify-doc", updated: true},
              sessionColl.findAndModify(
                  {query: {_id: "findAndModify-doc"}, update: {$set: {updated: true}}, new: true}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.findandmodify, sessionColl.getName(), tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));

    jsTestLog("Test geoSearch.");
    assert.commandWorked(
        sessionDB.runCommand({geoSearch: collName, near: [0, 0], maxDistance: 1, search: {a: 1}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.geoSearch, sessionColl.getName(), tojson(profileObj));

    jsTestLog("Test getMore.");
    let res = assert.commandWorked(
        sessionDB.runCommand({find: collName, filter: {_id: "read-doc"}, batchSize: 0}));
    assert(res.hasOwnProperty("cursor"), tojson(res));
    assert(res.cursor.hasOwnProperty("id"), tojson(res));
    let cursorId = res.cursor.id;
    res = assert.commandWorked(sessionDB.runCommand({getMore: cursorId, collection: collName}));
    assert.eq([{_id: "read-doc"}], res.cursor.nextBatch, tojson(res));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "getmore", tojson(profileObj));
    assert.eq(profileObj.nreturned, 1, tojson(profileObj));

    jsTestLog("Test insert.");
    assert.commandWorked(sessionColl.insert({_id: "insert-doc"}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "insert", tojson(profileObj));
    assert.eq(profileObj.ninserted, 1, tojson(profileObj));

    jsTestLog("Test update.");
    assert.commandWorked(sessionColl.update({_id: "update-doc"}, {$set: {updated: true}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "update", tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));

    jsTestLog("Test multi update.");
    assert.commandWorked(sessionColl.updateMany(
        {_id: {$in: ["multi-update-doc-1", "multi-update-doc-2"]}}, {$set: {updated: true}}));
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "update", tojson(profileObj));
    assert.eq(profileObj.nMatched, 2, tojson(profileObj));
    assert.eq(profileObj.nModified, 2, tojson(profileObj));

    jsTestLog("Test batch update.");
    assert.commandWorked(sessionDB.runCommand({
        update: collName,
        updates: [
            {q: {_id: "multi-update-doc-1"}, u: {$set: {batch_updated: true}}},
            {q: {_id: "multi-update-doc-2"}, u: {$set: {batch_updated: true}}}
        ]
    }));
    // We see the profile entry from the second update.
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "update", tojson(profileObj));
    assert.eq(profileObj.nMatched, 1, tojson(profileObj));
    assert.eq(profileObj.nModified, 1, tojson(profileObj));

    jsTestLog("Committing transaction.");
    session.commitTransaction();

    jsTestLog("Test delete with a write conflict.");
    assert.commandWorked(sessionColl.insert({_id: "delete-doc"}, {writeConcern: {w: "majority"}}));
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Perform an operation in the transaction to establish the snapshot.
    assert.eq(1, sessionColl.find({_id: "read-doc"}).itcount());

    // Update the document outside of the transaction.
    assert.commandWorked(testDB[collName].update({_id: "delete-doc"}, {$set: {conflict: true}}));

    // Deleting the document in the transaction fails, but profiling is still successful.
    assert.throws(function() {
        sessionColl.deleteOne({_id: "delete-doc"});
    });
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "remove", tojson(profileObj));
    assert.eq(profileObj.ndeleted, 0, tojson(profileObj));
    assert.eq(profileObj.errCode, ErrorCodes.WriteConflict, tojson(profileObj));
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test findAndModify with a write conflict.");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Perform an operation in the transaction to establish the snapshot.
    assert.eq(1, sessionColl.find({_id: "read-doc"}).itcount());

    // Update the document outside of the transaction.
    assert.commandWorked(
        testDB[collName].update({_id: "findAndModify-doc"}, {$set: {conflict: true}}));

    // Modifying the document in the transaction fails, but profiling is still successful.
    assert.throws(function() {
        sessionColl.findAndModify(
            {query: {_id: "findAndModify-doc"}, update: {$set: {conflict: false}}});
    });
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.op, "command", tojson(profileObj));
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.command.findandmodify, sessionColl.getName(), tojson(profileObj));
    assert.eq(profileObj.errCode, ErrorCodes.WriteConflict, tojson(profileObj));
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test insert with a write conflict.");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Perform an operation in the transaction to establish the snapshot.
    assert.eq(1, sessionColl.find({_id: "read-doc"}).itcount());

    // Insert a document outside of the transaction.
    assert.commandWorked(testDB[collName].insert({_id: "conflict-doc"}));

    // Inserting a document with the same _id in the transaction fails, but profiling is still
    // successful.
    assert.commandFailedWithCode(sessionColl.insert({_id: "conflict-doc"}),
                                 ErrorCodes.WriteConflict);
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "insert", tojson(profileObj));
    assert.eq(profileObj.ninserted, 0, tojson(profileObj));
    assert.eq(profileObj.errCode, ErrorCodes.WriteConflict, tojson(profileObj));
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    jsTestLog("Test update with a write conflict.");
    session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});

    // Perform an operation in the transaction to establish the snapshot.
    assert.eq(1, sessionColl.find({_id: "read-doc"}).itcount());

    // Update the document outside of the transaction.
    assert.commandWorked(testDB[collName].update({_id: "update-doc"}, {$set: {conflict: true}}));

    // Updating the document in the transaction fails, but profiling is still successful.
    assert.commandFailedWithCode(sessionColl.update({_id: "update-doc"}, {$set: {conflict: false}}),
                                 ErrorCodes.WriteConflict);
    profileObj = getLatestProfilerEntry(testDB);
    assert.eq(profileObj.ns, sessionColl.getFullName(), tojson(profileObj));
    assert.eq(profileObj.op, "update", tojson(profileObj));
    assert.eq(profileObj.errCode, ErrorCodes.WriteConflict, tojson(profileObj));
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.endSession();
}());
