// Tests the behaviour of change streams on change collections in an environment with more than one
// active tenant.
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

// Verify that the change stream observes expected events. The method also collects resume tokens
// for each expected change collection and returns those on successful assertion.
function verifyEventsAndGetResumeTokens(csCursor, expectedEvents) {
    let resumeTokens = [];

    for (const [expectedOpType, expectedPostImage, expectedPreImage] of expectedEvents) {
        assert.soon(() => csCursor.hasNext());
        const event = csCursor.next();

        assert.eq(event.operationType, expectedOpType, event);
        if (event.operationType == "insert") {
            assert.eq(event.fullDocument, expectedPostImage);
        } else if (event.operationType == "update") {
            assert.eq(event.fullDocumentBeforeChange, expectedPreImage, event);
            assert.eq(event.fullDocument, expectedPostImage, event);
        } else if (event.operationType == "drop") {
            assert.soon(() => csCursor.hasNext());
            assert.eq(csCursor.isClosed(), true);
        }

        resumeTokens.push(csCursor.getResumeToken());
    }

    return resumeTokens;
}

// Get the 'test' db for both tenants.
const firstTenantTestDb = firstTenantConn.getDB("test");
const secondTenantTestDb = secondTenantConn.getDB("test");

// Recreate the 'stockPrice' collections to delete any old documents and enable pre-images
// collections for them.
assertDropAndRecreateCollection(
    firstTenantTestDb, "stockPrice", {changeStreamPreAndPostImages: {enabled: true}});
assert(firstTenantTestDb.getCollectionInfos({name: "stockPrice"})[0]
           .options.changeStreamPreAndPostImages.enabled);
assertDropAndRecreateCollection(
    secondTenantTestDb, "stockPrice", {changeStreamPreAndPostImages: {enabled: true}});
assert(secondTenantTestDb.getCollectionInfos({name: "stockPrice"})[0]
           .options.changeStreamPreAndPostImages.enabled);

// Verify that while the change streams are disabled for the tenant, performing update and delete
// operations on a collection with change stream pre- and post-images enabled succeeds. The
// pre-images collection shouldn't be affected either.
replSetTest.setChangeStreamState(firstTenantConn, false);
assert.commandWorked(firstTenantTestDb.stockPrice.insert({_id: "mdb", price: 350}));
assert.commandWorked(firstTenantTestDb.stockPrice.updateOne({_id: "mdb"}, {$set: {price: 450}}));
assert.commandWorked(firstTenantTestDb.stockPrice.deleteOne({_id: "mdb"}));
assert(!firstTenantConn.getDB("config").getCollectionNames().includes("system.preimages"));

// Create a new incarnation of the change collection for the first tenant.
replSetTest.setChangeStreamState(firstTenantConn, false);
replSetTest.setChangeStreamState(firstTenantConn, true);

// These documents will be inserted in tenants 'stockPrice' collections.
const firstTenantDocs =
    [{_id: "mdb", price: 350}, {_id: "goog", price: 2000}, {_id: "nflx", price: 220}];
const secondTenantDocs =
    [{_id: "amzn", price: 3000}, {_id: "tsla", price: 750}, {_id: "aapl", price: 160}];

// Open the change stream cursor for the first tenant.
const firstTenantCsCursor = firstTenantTestDb.stockPrice.watch([]);

// Fetch the latest timestamp before enabling the change stream for the second tenant.
const startAtOperationTime =
    primary.getDB("local").oplog.rs.find().sort({ts: -1}).limit(1).next().ts;
assert(startAtOperationTime !== undefined);

// Now create the change collection for the second tenant. The oplog timestamp associated with the
// second tenant's create change collection will be greater than the 'startAtOperationTime'.
replSetTest.setChangeStreamState(secondTenantConn, false);
replSetTest.setChangeStreamState(secondTenantConn, true);

// Open the change stream cursor for the second tenant.
const secondTenantCsCursor = secondTenantTestDb.stockPrice.watch([]);

// Insert documents to both change collections in jumbled fashion.
assert.commandWorked(secondTenantTestDb.stockPrice.insert(secondTenantDocs[0]));
assert.commandWorked(firstTenantTestDb.stockPrice.insert(firstTenantDocs[0]));
assert.commandWorked(firstTenantTestDb.stockPrice.insert(firstTenantDocs[1]));
assert.commandWorked(secondTenantTestDb.stockPrice.insert(secondTenantDocs[1]));
assert.commandWorked(secondTenantTestDb.stockPrice.insert(secondTenantDocs[2]));
assert.commandWorked(firstTenantTestDb.stockPrice.insert(firstTenantDocs[2]));

// Verify that each change stream emits only the required tenant's change events and that there
// is no leak of events amongst the change streams. Do not consume all events for the first
// tenant as it will be consumed later.
const firstTenantResumeTokens = verifyEventsAndGetResumeTokens(
    firstTenantCsCursor, [["insert", firstTenantDocs[0]], ["insert", firstTenantDocs[1]]]);
const secondTenantResumeTokens = verifyEventsAndGetResumeTokens(secondTenantCsCursor, [
    ["insert", secondTenantDocs[0]],
    ["insert", secondTenantDocs[1]],
    ["insert", secondTenantDocs[2]]
]);

// Verify that change streams from both tenants can be resumed using their respective resume token.
verifyEventsAndGetResumeTokens(
    firstTenantTestDb.stockPrice.watch([], {resumeAfter: firstTenantResumeTokens[0]}),
    [["insert", firstTenantDocs[1]], ["insert", firstTenantDocs[2]]]);
verifyEventsAndGetResumeTokens(
    secondTenantTestDb.stockPrice.watch([], {resumeAfter: secondTenantResumeTokens[0]}),
    [["insert", secondTenantDocs[1]], ["insert", secondTenantDocs[2]]]);

// Verify that resume tokens cannot be exchanged between tenants change streams.
assert.throwsWithCode(
    () => secondTenantTestDb.stockPrice.watch([], {resumeAfter: firstTenantResumeTokens[0]}),
    ErrorCodes.ChangeStreamFatalError);
assert.throwsWithCode(
    () => firstTenantTestDb.stockPrice.watch([], {resumeAfter: secondTenantResumeTokens[0]}),
    ErrorCodes.ChangeStreamFatalError);

// Verify that the first tenant's change stream can be resumed using the timestamp
// 'startAtOperationTime'.
verifyEventsAndGetResumeTokens(
    firstTenantTestDb.stockPrice.watch([], {startAtOperationTime: startAtOperationTime}), [
        ["insert", firstTenantDocs[0]],
        ["insert", firstTenantDocs[1]],
        ["insert", firstTenantDocs[2]]
    ]);

// Verify that the second tenant's change stream cannot be resumed with the timestamp
// 'startAtOperationTime' and should throw change stream history lost.
assert.throwsWithCode(
    () => secondTenantTestDb.stockPrice.watch([], {startAtOperationTime: startAtOperationTime}),
    ErrorCodes.ChangeStreamHistoryLost);

// Ensure that disabling the change stream for the second tenant does not impact the change
// stream of the first tenant.
replSetTest.setChangeStreamState(secondTenantConn, false);

// The next on the change stream for the second tenant should now throw exception.
assert.throwsWithCode(() => assert.soon(() => secondTenantCsCursor.hasNext()),
                      ErrorCodes.QueryPlanKilled);

// The next of the change stream for the first tenant should continue to work. Since we have
// still not consumed all event from the first tenant, the change stream should emit the
// remaining ones.
verifyEventsAndGetResumeTokens(firstTenantCsCursor, [["insert", firstTenantDocs[2]]]);

// Re-enable the change stream for the second tenant and verify that the change stream cannot be
// resumed using the resume token of previous incarnation of the change stream.
replSetTest.setChangeStreamState(secondTenantConn, true);
assert.throwsWithCode(
    () => secondTenantTestDb.stockPrice.watch([], {resumeAfter: secondTenantResumeTokens[0]}),
    ErrorCodes.ChangeStreamHistoryLost);

const firstTenantCsCursorWithPreAndPostImages = firstTenantTestDb.stockPrice.watch(
    [], {fullDocument: "required", fullDocumentBeforeChange: "required"});
const secondTenantCsCursorWithPreAndPostImages = secondTenantTestDb.stockPrice.watch(
    [], {fullDocument: "required", fullDocumentBeforeChange: "required"});

assert.commandWorked(firstTenantTestDb.stockPrice.updateOne({_id: "mdb"}, {$set: {price: 450}}));
assert.commandWorked(secondTenantTestDb.stockPrice.updateOne({_id: "amzn"}, {$set: {price: 300}}));

verifyEventsAndGetResumeTokens(firstTenantCsCursorWithPreAndPostImages,
                               [["update", {_id: "mdb", price: 450}, {_id: "mdb", price: 350}]]);
verifyEventsAndGetResumeTokens(secondTenantCsCursorWithPreAndPostImages,
                               [["update", {_id: "amzn", price: 300}, {_id: "amzn", price: 3000}]]);

replSetTest.stopSet();
}());
