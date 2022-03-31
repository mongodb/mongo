/**
 * Tests for aggregation requests with the collectionUUID parameter.
 * @tags: [
 *   # Change stream aggregations don't support read concerns other than 'majority'
 *   assumes_read_concern_unchanged,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'isMongos'

const dbName = jsTestName();
const collName = "foo";

const testDB = db.getSiblingDB(dbName);
const testColl = testDB.getCollection(collName);

const validateErrorResponse = function(
    res, db, collectionUUID, expectedCollection, actualCollection) {
    assert.eq(res.db, db);
    assert.eq(res.collectionUUID, collectionUUID);
    assert.eq(res.expectedCollection, expectedCollection);
    assert.eq(res.actualCollection, actualCollection);
};

const docs = [{_id: 1}, {_id: 2}];

assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testColl.insert(docs));

// Get the namespace's initial UUID.
let collectionInfos = testDB.getCollectionInfos({name: collName});
let uuid = collectionInfos[0].info.uuid;
assert(uuid, "Expected collection " + collName + " to have a UUID.");

// An aggregation with the UUID should succeed and find the same documents as an aggregation with
// the collection name.
let uuidRes = assert.commandWorked(testDB.runCommand(
    {aggregate: collName, collectionUUID: uuid, pipeline: [{$match: {}}], cursor: {}}));
assert.sameMembers(uuidRes.cursor.firstBatch, docs);

let collNameRes = assert.commandWorked(
    testDB.runCommand({aggregate: collName, pipeline: [{$match: {}}], cursor: {}}));
assert.sameMembers(collNameRes.cursor.firstBatch, uuidRes.cursor.firstBatch);

// getMore should work with cursors created by an aggregation with a uuid.
uuidRes = assert.commandWorked(testDB.runCommand(
    {aggregate: collName, pipeline: [{$match: {}}, {$sort: {_id: 1}}], cursor: {batchSize: 1}}));
assert.eq(1, uuidRes.cursor.firstBatch.length, tojson(uuidRes));
assert.eq(docs[0], uuidRes.cursor.firstBatch[0], tojson(uuidRes));

const getMoreRes =
    assert.commandWorked(testDB.runCommand({getMore: uuidRes.cursor.id, collection: collName}));
assert.eq(1, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));
assert.eq(docs[1], getMoreRes.cursor.nextBatch[0], tojson(getMoreRes));
assert.eq(0, getMoreRes.cursor.id, tojson(getMoreRes));

// An aggregation with collectionUUID throws NamespaceNotFound if the namespace does not exist, even
// if a collection does exist with the given uuid.
let res = assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: "doesNotExist", collectionUUID: uuid, pipeline: [{$match: {}}], cursor: {}}),
    ErrorCodes.CollectionUUIDMismatch);
validateErrorResponse(res, dbName, uuid, 'doesNotExist', testColl.getName());

// Drop the collection.
testColl.drop({writeConcern: {w: "majority"}});

// An aggregation with the initial UUID should fail since the namespace doesn't exist.
res = assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: collName, collectionUUID: uuid, pipeline: [{$match: {}}], cursor: {}}),
    ErrorCodes.CollectionUUIDMismatch);
validateErrorResponse(res, dbName, uuid, testColl.getName(), null);

// Now recreate the collection.
assert.commandWorked(testColl.insert(docs));

// An aggregation with the initial UUID should still fail despite the namespace existing.
res = assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: collName, collectionUUID: uuid, pipeline: [{$match: {}}], cursor: {}}),
    ErrorCodes.CollectionUUIDMismatch);
validateErrorResponse(res, dbName, uuid, testColl.getName(), null);

collNameRes = assert.commandWorked(
    testDB.runCommand({aggregate: collName, pipeline: [{$match: {}}], cursor: {}}));
assert.sameMembers(collNameRes.cursor.firstBatch, docs);

// An aggregation with a collectionUUID should fail with CollectionUUIDMismatch if the namespace is
// a view.
const viewName = 'view';
assert.commandWorked(testDB.runCommand(
    {create: viewName, viewOn: testColl.getName(), pipeline: [], writeConcern: {w: "majority"}}));

res = assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: "viewCollection", collectionUUID: uuid, pipeline: [{$match: {}}], cursor: {}}),
    ErrorCodes.CollectionUUIDMismatch);
validateErrorResponse(res, dbName, uuid, 'viewCollection', null);

//
// Tests for rejecting invalid collectionUUIDs and cases where collectionUUID is not allowed.
//

// collectionUUID must be a UUID.
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: collName, collectionUUID: "NotAUUID", pipeline: [{$match: {}}], cursor: {}}),
    ErrorCodes.TypeMismatch);

// collectionUUID is not allowed with change streams.
assert.commandFailedWithCode(
    testDB.runCommand(
        {aggregate: collName, collectionUUID: uuid, pipeline: [{$changeStream: {}}], cursor: {}}),
    4928900);

// collectionUUID is not allowed with collectionless aggregations.
assert.commandFailedWithCode(
    testDB.adminCommand(
        {aggregate: 1, collectionUUID: uuid, pipeline: [{$currentOp: {}}], cursor: {}}),
    4928901);
})();
