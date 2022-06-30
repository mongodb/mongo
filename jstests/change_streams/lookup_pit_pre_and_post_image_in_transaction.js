/**
 * Tests that point-in-time pre- and post-images are retrieved for update/replace/delete operations
 * performed in a transaction and non-atomic "applyOps" command.
 * @tags: [
 * requires_fcv_60,
 * featureFlagChangeStreamPreAndPostImages,
 * uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");        // For isChangeStreamPreAndPostImagesEnabled and
                                                   // ChangeStreamTest.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/fixture_helpers.js");           // For FixtureHelpers.isMongos.
load("jstests/libs/transactions_util.js");         // For TransactionsUtil.runInTransaction.

const testDB = db.getSiblingDB(jsTestName());
const cst = new ChangeStreamTest(testDB);
const coll = assertDropAndRecreateCollection(
    testDB, "coll", {changeStreamPreAndPostImages: {enabled: true}});
const collOther = assertDropAndRecreateCollection(testDB, "coll_regular");

// Verifies that change stream cursor 'changeStreamCursor' returns events defined in array
// 'expectedEvents' in any order.
function assertChangeEventsReturned(changeStreamCursor, expectedEvents) {
    function toChangeEvent(event) {
        const {_id, operationType, preImage, postImage} = event;
        let result = {
            documentKey: {_id},
            ns: {db: testDB.getName(), coll: coll.getName()},
            operationType,
        };
        if (preImage != undefined) {
            result.fullDocumentBeforeChange = preImage;
        }
        if (postImage != undefined) {
            result.fullDocument = postImage;
        }
        return result;
    }
    cst.assertNextChangesEqualUnordered(
        {cursor: changeStreamCursor, expectedChanges: expectedEvents.map(toChangeEvent)});
}

assert.commandWorked(coll.insert([{_id: 1, a: 1}, {_id: 2, a: 1}, {_id: 3, a: 1}]));

// Open a change stream on the test collection with pre- and post-images requested.
const changeStreamCursor = cst.startWatchingChanges({
    pipeline: [
        {$changeStream: {fullDocumentBeforeChange: 'whenAvailable', fullDocument: 'whenAvailable'}}
    ],
    collection: coll
});

// Gets collections used in the test for database 'db'. In some passthroughs the collections get
// sharded on 'getCollection()' invocation and it must happen when a transaction is not active.
function getCollections(db) {
    return {coll: db[coll.getName()], otherColl: db[collOther.getName()]};
}

jsTestLog("Testing a transaction consisting of a single 'applyOps' entry.");
TransactionsUtil.runInTransaction(testDB, getCollections, function(db, {coll, otherColl}) {
    assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));
    assert.commandWorked(coll.replaceOne({_id: 2}, {a: "Long string"}));
    assert.commandWorked(coll.deleteOne({_id: 3}));
});
assertChangeEventsReturned(changeStreamCursor, [
    {_id: 1, operationType: "update", preImage: {_id: 1, a: 1}, postImage: {_id: 1, a: 2}},
    {
        _id: 2,
        operationType: "replace",
        preImage: {_id: 2, a: 1},
        postImage: {_id: 2, a: "Long string"}
    },
    {_id: 3, operationType: "delete", preImage: {_id: 3, a: 1}},
]);

jsTestLog("Testing a transaction consisting of multiple 'applyOps' entries.");
const largeStringSizeInBytes = 15 * 1024 * 1024;
const largeString = "b".repeat(largeStringSizeInBytes);
assert.commandWorked(coll.insert([{_id: 3, a: 1}]));
TransactionsUtil.runInTransaction(testDB, getCollections, function(db, {coll, otherColl}) {
    assert.commandWorked(otherColl.insert({b: largeString}));
    assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));

    assert.commandWorked(otherColl.insert({b: largeString}));
    assert.commandWorked(coll.replaceOne({_id: 2}, {a: 1}));

    // Issue a second modification operation on the same document within the transaction.
    assert.commandWorked(coll.updateOne({_id: 2}, {$inc: {a: 1}}));

    assert.commandWorked(coll.deleteOne({_id: 3}));
    assert.commandWorked(otherColl.insert({b: largeString}));
});
assertChangeEventsReturned(changeStreamCursor, [
    {_id: 3, operationType: "insert", postImage: {_id: 3, a: 1}},
    {_id: 1, operationType: "update", preImage: {_id: 1, a: 2}, postImage: {_id: 1, a: 3}},
    {
        _id: 2,
        operationType: "replace",
        preImage: {_id: 2, a: "Long string"},
        postImage: {_id: 2, a: 1}
    },
    {_id: 2, operationType: "update", preImage: {_id: 2, a: 1}, postImage: {_id: 2, a: 2}},
    {_id: 3, operationType: "delete", preImage: {_id: 3, a: 1}},
]);

jsTestLog("Testing a transaction consisting of multiple 'applyOps' entries with large pre-images.");
const largePreImageSizeInBytes = 7 * 1024 * 1024;
const largePreImageValue = "c".repeat(largePreImageSizeInBytes);
assert.commandWorked(coll.insert([{_id: 3, a: largePreImageValue}]));
TransactionsUtil.runInTransaction(testDB, getCollections, function(db, {coll, otherColl}) {
    assert.commandWorked(coll.updateOne({_id: 3}, {$set: {b: 1}}));
    assert.commandWorked(coll.deleteOne({_id: 3}));
});
assertChangeEventsReturned(changeStreamCursor, [
    {_id: 3, operationType: "insert", postImage: {_id: 3, a: largePreImageValue}},
    {
        _id: 3,
        operationType: "update",
        preImage: {_id: 3, a: largePreImageValue},
        postImage: {_id: 3, a: largePreImageValue, b: 1}
    },
    {
        _id: 3,
        operationType: "delete",
        preImage: {_id: 3, a: largePreImageValue, b: 1},
    },
]);

// "applyOps" command can only be issued on a replica set.
if (!FixtureHelpers.isMongos(testDB)) {
    jsTestLog("Testing non-atomic 'applyOps' command.");
    assert.commandWorked(coll.insert([{_id: 5, a: 1}, {_id: 6, a: 1}]));
    assert.commandWorked(testDB.runCommand({
        applyOps: [
            {op: "u", ns: coll.getFullName(), o2: {_id: 5}, o: {$v: 2, diff: {u: {a: 2}}}},
            {op: "d", ns: coll.getFullName(), o: {_id: 6}}
        ],
        allowAtomic: false,
    }));
    assertChangeEventsReturned(changeStreamCursor, [
        {_id: 5, operationType: "insert", postImage: {_id: 5, a: 1}},
        {_id: 6, operationType: "insert", postImage: {_id: 6, a: 1}},
        {_id: 5, operationType: "update", preImage: {_id: 5, a: 1}, postImage: {_id: 5, a: 2}},
        {_id: 6, operationType: "delete", preImage: {_id: 6, a: 1}},
    ]);
}
})();