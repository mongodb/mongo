/**
 * Verify that transactions correctly abort on unique index write conflicts.
 *
 *  @tags: [uses_transactions]
 */

(function() {
    "use strict";

    load("jstests/core/txns/libs/write_conflicts.js");  // for 'WriteConflictHelpers'.

    const dbName = "test";
    const collName = "transactions_write_conflicts_unique_indexes";

    const testDB = db.getSiblingDB(dbName);
    const coll = testDB[collName];

    // Clean up and create test collection.
    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    testDB.runCommand({create: coll.getName(), writeConcern: {w: "majority"}});

    // Create a unique index on field 'x'.
    assert.commandWorked(coll.createIndex({x: 1}, {unique: true}));

    /***********************************************************************************************
     * Single document conflicts.
     **********************************************************************************************/

    jsTestLog("Test single document write conflicts.");

    print("insert-insert conflict.");

    let t1Op = {insert: collName, documents: [{_id: 1, x: 1}]};
    let t2Op = {insert: collName, documents: [{_id: 2, x: 1}]};
    let expectedDocs1 = [{_id: 1, x: 1}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs1, WriteConflictHelpers.T1StartsFirstAndWins);
    let expectedDocs2 = [{_id: 2, x: 1}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs2, WriteConflictHelpers.T2StartsSecondAndWins);

    print("update-update conflict");
    let initOp = {
        insert: collName,
        documents: [{_id: 1, x: 1}, {_id: 2, x: 2}]
    };  // the document to update.
    t1Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {x: 3}}}]};
    t2Op = {update: collName, updates: [{q: {_id: 2}, u: {$set: {x: 3}}}]};
    expectedDocs1 = [{_id: 1, x: 3}, {_id: 2, x: 2}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs1, WriteConflictHelpers.T1StartsFirstAndWins, initOp);
    expectedDocs2 = [{_id: 1, x: 1}, {_id: 2, x: 3}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs2, WriteConflictHelpers.T2StartsSecondAndWins, initOp);

    print("upsert-upsert conflict");
    t1Op = {update: collName, updates: [{q: {_id: 1}, u: {$set: {x: 1}}, upsert: true}]};
    t2Op = {update: collName, updates: [{q: {_id: 2}, u: {$set: {x: 1}}, upsert: true}]};
    expectedDocs1 = [{_id: 1, x: 1}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs1, WriteConflictHelpers.T1StartsFirstAndWins);
    expectedDocs2 = [{_id: 2, x: 1}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs2, WriteConflictHelpers.T2StartsSecondAndWins);

    /***********************************************************************************************
     * Multi-document and predicate based conflicts.
     **********************************************************************************************/

    jsTestLog("Test multi-document and predicate based write conflicts.");

    print("batch insert-batch insert conflict");
    t1Op = {insert: collName, documents: [{_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]};
    t2Op = {insert: collName, documents: [{_id: 4, x: 2}, {_id: 5, x: 3}, {_id: 6, x: 4}]};
    expectedDocs1 = [{_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs1, WriteConflictHelpers.T1StartsFirstAndWins);
    expectedDocs2 = [{_id: 4, x: 2}, {_id: 5, x: 3}, {_id: 6, x: 4}];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs2, WriteConflictHelpers.T2StartsSecondAndWins);

    print("multiupdate-multiupdate conflict");
    // Update disjoint sets of documents such that the post-image of each set would create a unique
    // index violation.
    initOp = {
        insert: collName,
        documents: [
            // Set 1
            {_id: 1, x: 1},
            {_id: 2, x: 2},
            {_id: 3, x: 3},
            // Set 2
            {_id: 4, x: 10},
            {_id: 5, x: 11},
            {_id: 6, x: 12}
        ]  // the documents to update.
    };
    t1Op = {update: collName, updates: [{q: {_id: {$lte: 3}}, u: {$inc: {x: 4}}, multi: true}]};
    t2Op = {update: collName, updates: [{q: {_id: {$gte: 4}}, u: {$inc: {x: -4}}, multi: true}]};
    expectedDocs1 = [
        {_id: 1, x: 5},
        {_id: 2, x: 6},
        {_id: 3, x: 7},
        {_id: 4, x: 10},
        {_id: 5, x: 11},
        {_id: 6, x: 12}
    ];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs1, WriteConflictHelpers.T1StartsFirstAndWins, initOp);
    expectedDocs2 = [
        {_id: 1, x: 1},
        {_id: 2, x: 2},
        {_id: 3, x: 3},
        {_id: 4, x: 6},
        {_id: 5, x: 7},
        {_id: 6, x: 8}
    ];
    WriteConflictHelpers.writeConflictTest(
        coll, t1Op, t2Op, expectedDocs2, WriteConflictHelpers.T2StartsSecondAndWins, initOp);

}());
