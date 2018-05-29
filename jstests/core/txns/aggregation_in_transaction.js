// Tests that aggregation is supported in transactions.
// @tags: [uses_transactions]
(function() {
    "use strict";

    const session = db.getMongo().startSession({causalConsistency: false});
    const testDB = session.getDatabase("test");
    const coll = testDB.getCollection("aggregation_in_transaction");
    const foreignColl = testDB.getCollection("aggregation_in_transaction_lookup");

    [coll, foreignColl].forEach(col => {
        const reply = col.runCommand("drop", {writeConcern: {w: "majority"}});
        if (reply.ok !== 1) {
            assert.commandFailedWithCode(reply, ErrorCodes.NamespaceNotFound);
        }
    });

    // Populate the collections.
    const testDoc = {_id: 0, foreignKey: "orange"};
    assert.commandWorked(coll.insert(testDoc, {writeConcern: {w: "majority"}}));
    const foreignDoc = {_id: "orange", val: 9};
    assert.commandWorked(foreignColl.insert(foreignDoc, {writeConcern: {w: "majority"}}));

    // Run a dummy find to start the transaction.
    jsTestLog("Starting transaction.");
    session.startTransaction({readConcern: {level: "snapshot"}});
    let cursor = coll.find();
    cursor.next();

    // Insert a document outside of the transaction. Subsequent aggregations should not see this
    // document.
    jsTestLog("Inserting document outside of transaction.");
    assert.commandWorked(db.getSiblingDB(testDB.getName()).getCollection(coll.getName()).insert({
        _id: "not_visible_in_transaction",
        foreignKey: "orange",
    }));

    // Perform an aggregation that is fed by a cursor on the underlying collection. Only the
    // majority-committed document present at the start of the transaction should be found.
    jsTestLog("Starting aggregations inside of the transaction.");
    cursor = coll.aggregate({$match: {}});
    assert.docEq(testDoc, cursor.next());
    assert(!cursor.hasNext());

    // Perform aggregations that look at other collections.
    const lookupDoc = Object.extend(testDoc, {lookup: [foreignDoc]});
    cursor = coll.aggregate({
        $lookup: {
            from: foreignColl.getName(),
            localField: "foreignKey",
            foreignField: "_id",
            as: "lookup",
        }
    });
    assert.docEq(cursor.next(), lookupDoc);
    assert(!cursor.hasNext());

    cursor = coll.aggregate({
        $graphLookup: {
            from: foreignColl.getName(),
            startWith: "$foreignKey",
            connectFromField: "foreignKey",
            connectToField: "_id",
            as: "lookup"
        }
    });
    assert.docEq(cursor.next(), lookupDoc);
    assert(!cursor.hasNext());

    jsTestLog("Testing $count within a transaction.");

    let countRes = coll.aggregate([{$count: "count"}]).toArray();
    assert.eq(countRes.length, 1, tojson(countRes));
    assert.eq(countRes[0].count, 1, tojson(countRes));

    assert.commandWorked(coll.insert({a: 2}));
    countRes = coll.aggregate([{$count: "count"}]).toArray();
    assert.eq(countRes.length, 1, tojson(countRes));
    assert.eq(countRes[0].count, 2, tojson(countRes));

    assert.commandWorked(
        db.getSiblingDB(testDB.getName()).getCollection(coll.getName()).insert({a: 3}));
    countRes = coll.aggregate([{$count: "count"}]).toArray();
    assert.eq(countRes.length, 1, tojson(countRes));
    assert.eq(countRes[0].count, 2, tojson(countRes));

    session.commitTransaction();
    jsTestLog("Transaction committed.");

    // Perform aggregations with non-cursor initial sources and assert that they are not supported
    // in transactions.
    jsTestLog("Running aggregations in transactions that are expected to throw and fail.");
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.throws(() => coll.aggregate({$currentOp: {allUsers: true, localOps: true}}).next());
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.throws(
        () => coll.aggregate({$collStats: {latencyStats: {histograms: true}, storageStats: {}}})
                  .next());
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.throws(() => coll.aggregate({$indexStats: {}}).next());
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);
}());
