// Tests the behaviour of change streams on change collections with transaction in an environment
// with more than one active tenant.
// @tags: [
//   requires_fcv_62,
//   assumes_against_mongod_not_mongos,
// ]

(function() {
"use strict";

// For ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");
// For assertDropAndRecreateCollection.
load("jstests/libs/collection_drop_recreate.js");

// Verify that the change stream observes expected events. The method also collects resume tokens
// for each expected change collection and returns those on successful assertion.
function verifyEventsAndGetResumeTokens(csCursor, expectedEvents) {
    for (const [expectedOpType, expectedPostImage, expectedPreImage] of expectedEvents) {
        assert.soon(() => csCursor.hasNext());
        const event = csCursor.next();

        assert.eq(event.operationType, expectedOpType, event);
        if (event.operationType == "insert") {
            assert.eq(event.fullDocument, expectedPostImage);
        } else if (event.operationType == "update") {
            assert.eq(event.fullDocumentBeforeChange, expectedPreImage, event);
            assert.eq(event.fullDocument, expectedPostImage, event);
        }
    }
}

const replSetTest = new ChangeStreamMultitenantReplicaSetTest({nodes: 2});
const primary = replSetTest.getPrimary();

// Hard code tenants ids such that a particular tenant can be identified deterministically.
const firstTenantId = ObjectId("6303b6bb84305d2266d0b779");
const secondTenantId = ObjectId("7303b6bb84305d2266d0b779");

// Connections to the replica set primary that are stamped with their respective tenant ids.
const firstTenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, firstTenantId);
const secondTenantConn =
    ChangeStreamMultitenantReplicaSetTest.getTenantConnection(primary.host, secondTenantId);

// Get the 'test' db for both tenants.
const firstTenantTestDb = firstTenantConn.getDB("test");
const secondTenantTestDb = secondTenantConn.getDB("test");

// Recreate the 'stockPrice' collections and enable pre-images collection for the first tenant.
assertDropAndRecreateCollection(
    firstTenantTestDb, "stockPrice", {changeStreamPreAndPostImages: {enabled: true}});
assert(firstTenantTestDb.getCollectionInfos({name: "stockPrice"})[0]
           .options.changeStreamPreAndPostImages.enabled);

// Recreate the 'stockPrice' collections and enable pre-images collection for the second tenant.
assertDropAndRecreateCollection(
    secondTenantTestDb, "stockPrice", {changeStreamPreAndPostImages: {enabled: true}});
assert(secondTenantTestDb.getCollectionInfos({name: "stockPrice"})[0]
           .options.changeStreamPreAndPostImages.enabled);

// Create a new incarnation of the change collection for both tenants.
replSetTest.setChangeStreamState(firstTenantConn, true);
replSetTest.setChangeStreamState(secondTenantConn, true);

// Open the change stream cursors with pre-and-post images enabled.
const firstTenantCsCursor = firstTenantTestDb.stockPrice.watch(
    [], {fullDocumentBeforeChange: "required", fullDocument: "required"});
const secondTenantCsCursor = secondTenantTestDb.stockPrice.watch(
    [], {fullDocumentBeforeChange: "required", fullDocument: "required"});

// Enable transaction to perform writes within a transaction for the first tenant.
const firstTenantSession = firstTenantConn.getDB("test").getMongo().startSession();
const firstTenantSessionDb = firstTenantSession.getDatabase("test");
firstTenantSession.startTransaction();

// Enable transaction to perform writes within a transaction for the second second.
const secondTenantSession = secondTenantConn.getDB("test").getMongo().startSession();
const secondTenantSessionDb = secondTenantSession.getDatabase("test");
secondTenantSession.startTransaction();

// Perform inserts and updates in jumbled fashion for both tenants within the transaction.
assert.commandWorked(secondTenantSessionDb.stockPrice.insert({_id: "amzn", price: 3000}));
assert.commandWorked(firstTenantSessionDb.stockPrice.insert({_id: "mdb", price: 350}));
assert.commandWorked(firstTenantSessionDb.stockPrice.insert({_id: "goog", price: 2000}));
assert.commandWorked(secondTenantSessionDb.stockPrice.insert({_id: "tsla", price: 750}));
assert.commandWorked(secondTenantSessionDb.stockPrice.update({_id: "tsla"}, {$set: {price: 200}}));
assert.commandWorked(firstTenantSessionDb.stockPrice.update({_id: "mdb"}, {$set: {price: 190}}));

// Commit the transaction for both tenants.
firstTenantSession.commitTransaction_forTesting();
secondTenantSession.commitTransaction_forTesting();

// Verify change events for both tenants.
verifyEventsAndGetResumeTokens(firstTenantCsCursor, [
    ["insert", {_id: "mdb", price: 350}],
    ["insert", {_id: "goog", price: 2000}],
    ["update", {_id: "mdb", price: 190}, {_id: "mdb", price: 350}]
]);
verifyEventsAndGetResumeTokens(secondTenantCsCursor, [
    ["insert", {_id: "amzn", price: 3000}],
    ["insert", {_id: "tsla", price: 750}],
    ["update", {_id: "tsla", price: 200}, {_id: "tsla", price: 750}]
]);

replSetTest.stopSet();
}());
