/**
 * Test the behavior of aggregation with prepare conflicts. Reads from an aggregate pipeline
 * should not block on prepare conflicts, but writing out to a collection as a part of an aggregate
 * pipeline should block on prepare conflicts.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");

    const failureTimeout = 5 * 1000;  // 5 seconds.
    const dbName = "test";
    const collName = "prepare_conflict_aggregation_behavior";
    const outCollName = collName + "_out";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);
    const outColl = testDB.getCollection(outCollName);

    testColl.drop({writeConcern: {w: "majority"}});
    outColl.drop({writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));
    assert.commandWorked(testDB.runCommand({create: outCollName, writeConcern: {w: "majority"}}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collName);
    const sessionOutColl = sessionDB.getCollection(outCollName);

    assert.commandWorked(testColl.insert({_id: 1}));
    assert.commandWorked(outColl.insert({_id: 0}));

    session.startTransaction();
    assert.commandWorked(sessionColl.update({_id: 1}, {a: 1}));
    assert.commandWorked(sessionOutColl.update({_id: 0}, {a: 1}));
    let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    jsTestLog("Test that reads from an aggregation pipeline with $merge don't block on prepare" +
              " conflicts");
    testColl.aggregate([
        {$addFields: {b: 1}},
        {$merge: {into: outCollName, whenMatched: "fail", whenNotMatched: "insert"}}
    ]);

    // Make sure that we can see the inserts from the aggregation but not the updates from the
    // prepared transaction.
    assert.eq([{_id: 0}, {_id: 1, b: 1}], outColl.find().toArray());

    session.abortTransaction_forTesting();
    session.startTransaction();
    assert.commandWorked(sessionOutColl.update({_id: 1}, {_id: 1, a: 1}));
    prepareTimestamp = PrepareHelpers.prepareTransaction(session);

    jsTestLog("Test that writes from an aggregation pipeline block on prepare conflicts");
    let pipeline = [
        {$addFields: {c: 1}},
        {$merge: {into: outCollName, whenMatched: "replaceWithNew", whenNotMatched: "insert"}}
    ];
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: collName,
        pipeline: pipeline,
        cursor: {},
        maxTimeMS: failureTimeout,
    }),
                                 ErrorCodes.MaxTimeMSExpired);

    // Make sure that we can't see the update from the aggregation or the prepared transaction.
    assert.eq([{_id: 0}, {_id: 1, b: 1}], outColl.find().toArray());

    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    // Make sure that the $merge pipeline works once the transaction is committed.
    testColl.aggregate(pipeline);
    assert.eq([{_id: 0}, {_id: 1, c: 1}], outColl.find().toArray());

    // At the time of this writing, change streams can sometimes adjust the readConcern to
    // 'majority' after receiving the command and thus need to wait for read concern again. When
    // doing this, we assume that a change stream with a stage which performs writes is not allowed.
    // Test that this is true.
    pipeline = [{$changeStream: {}}, {$addFields: {d: 1}}, {$out: outCollName}];
    assert.commandFailedWithCode(testDB.runCommand({
        aggregate: collName,
        pipeline: pipeline,
        cursor: {},
        maxTimeMS: failureTimeout,
    }),
                                 ErrorCodes.IllegalOperation);

    session.endSession();
}());
