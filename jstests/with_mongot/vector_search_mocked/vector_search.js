/**
 * Tests for the `$vectorSearch` aggregation pipeline stage.
 * @tags: [
 *  requires_fcv_71,
 * ]
 */
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = jsTestName();
const collName = jsTestName();
const collNS = dbName + "." + collName;

// Start mock mongot.
const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

// Start mongod.
const conn = MongoRunner.runMongod({
    setParameter: {mongotHost: mockConn.host},
    verbose: 1,
});
const testDB = conn.getDB(dbName);
assertCreateCollection(testDB, collName);
const collectionUUID = getUUIDFromListCollections(testDB, collName);

const coll = testDB.getCollection(collName);
coll.insert({_id: 0});

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = 10;
const limit = 5;
const index = "index";

const cursorId = NumberLong(123);
const responseOk = 1;

// $vectorSearch does nothing on an empty collection.
(function testVectorSearchEmptyCollection() {
    const pipeline = [{$vectorSearch: {queryVector, path, limit}}];
    assert.eq(testDB.emptyCollection.aggregate(pipeline).toArray(), []);
})();

const someScore = {
    $vectorSearchScore: 0.99
};
// $vectorSearch can query mongot and correctly pass along results.
(function testVectorSearchQueriesMongotAndReturnsResults() {
    const filter = {x: {$gt: 0}};
    const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit, index, filter}}];

    const mongotResponseBatch = [{_id: 0, ...someScore}];
    const expectedDocs = [{_id: 0}];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            numCandidates,
            index,
            limit,
            collName,
            filter,
            dbName,
            collectionUUID
        }),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// $vectorSearch only returns # limit documents.
(function testVectorSearchRespectsLimit() {
    const pipeline = [{$vectorSearch: {queryVector, path, limit: 1}}];

    const mongotResponseBatch = [{_id: 0, ...someScore}, {_id: 1, ...someScore}];
    const expectedDocs = [{_id: 0}];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {queryVector, path, limit: 1, collName, dbName, collectionUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// $vectorSearch succeeds if a filter is present. Note that the filter will not be applied in
// this test, as the mock can't interpret MatchExpressions.
(function testVectorSearchWorksWithFilter() {
    const filter = {"$or": [{"color": {"$gt": "C"}}, {"color": {"$lt": "C"}}]};
    const pipeline = [{$vectorSearch: {queryVector, path, limit: 1, filter: filter}}];

    const mongotResponseBatch = [{_id: 0, ...someScore}, {_id: 1, ...someScore}];
    const expectedDocs = [{_id: 0}];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            limit: 1,
            index: null,
            filter: filter,
            collName,
            dbName,
            collectionUUID
        }),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// $vectorSearch populates {$meta: score}.
(function testVectorSearchPopulatesScoreMetaField() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit}},
        {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}}
    ];
    const mongotResponseBatch = [{_id: 0, $vectorSearchScore: 1.234}];
    const expectedDocs = [{_id: 0, score: 1.234}];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// Test that all fields are passed through to mongot.
(function testVectorSearchPassesAllFields() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit, "extraField": true}},
        {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}}
    ];
    const mongotResponseBatch = [{_id: 0, $vectorSearchScore: 1.234}];
    const expectedDocs = [{_id: 0, score: 1.234}];

    let localExpectedCommand = mongotCommandForVectorSearchQuery(
        {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID});
    localExpectedCommand["extraField"] = true;
    const history = [{
        expectedCommand: localExpectedCommand,
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// $vectorSearch handles errors returned by mongot.
(function testVectorSearchPropagatesMongotError() {
    const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit}}];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID}),
        response: {
            ok: 0,
            errmsg: "mongot error",
            code: ErrorCodes.InternalError,
            codeName: "InternalError"
        }
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.commandFailedWithCode(testDB.runCommand({aggregate: collName, pipeline, cursor: {}}),
                                 ErrorCodes.InternalError);
})();

coll.insert({_id: 1});
coll.insert({_id: 10});
coll.insert({_id: 11});
coll.insert({_id: 20});

// $vectorSearch handles multiple documents and batches correctly.
(function testVectorSearchMultipleBatches() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit}},
        {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}}
    ];

    const batchOne = [{_id: 0, $vectorSearchScore: 1.234}, {_id: 1, $vectorSearchScore: 1.21}];
    const batchTwo = [{_id: 10, $vectorSearchScore: 1.1}, {_id: 11, $vectorSearchScore: 0.8}];
    const batchThree = [{_id: 20, $vectorSearchScore: 0.2}];

    const expectedDocs = [
        {_id: 0, score: 1.234},
        {_id: 1, score: 1.21},
        {_id: 10, score: 1.1},
        {_id: 11, score: 0.8},
        {_id: 20, score: 0.2},
    ];

    const history = [
        {
            expectedCommand: mongotCommandForVectorSearchQuery(
                {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID}),
            response: mongotResponseForBatch(batchOne, cursorId, collNS, 1),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(batchTwo, cursorId, collNS, 1),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(batchThree, NumberLong(0), collNS, 1),
        }
    ];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline, {cursor: {batchSize: 2}}).toArray(),
              expectedDocs);
})();

// $vectorSearch handles errors returned by mongot during a getMore.
(function testVectorSearchPropagatesMongotGetMoreError() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit}},
        {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}}
    ];

    const batchOne = [{_id: 0, $vectorSearchScore: 1.234}, {_id: 1, $vectorSearchScore: 1.21}];

    const history = [
        {
            expectedCommand: mongotCommandForVectorSearchQuery(
                {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID}),
            response: mongotResponseForBatch(batchOne, cursorId, collNS, 1),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: {
                ok: 0,
                errmsg: "mongot error",
                code: ErrorCodes.InternalError,
                codeName: "InternalError"
            }
        }
    ];
    mongotMock.setMockResponses(history, cursorId);

    // The aggregate() (and search command) should succeed.
    // Note that 'batchSize' here only tells mongod how many docs to return per batch and has
    // no effect on the batches between mongod and mongotmock.
    const kBatchSize = 2;
    const cursor = coll.aggregate(pipeline, {batchSize: kBatchSize});

    // Iterate the first batch until it is exhausted.
    for (let i = 0; i < kBatchSize; i++) {
        cursor.next();
    }

    // The next call to next() will result in a 'getMore' being sent to mongod. $vectorSearch's
    // internal cursor to mongot will have no results left, and thus, a 'getMore' will be sent
    // to mongot. The error should propagate back to the client.
    const err = assert.throws(() => cursor.next());
    assert.commandFailedWithCode(err, ErrorCodes.InternalError);
})();

coll.insert({_id: 2, x: "now", y: "lorem"});
coll.insert({_id: 3, x: "brown", y: "ipsum"});
coll.insert({_id: 4, x: "cow", y: "lorem ipsum"});

// $vectorSearch uses the idLookup stage as expected.
(function testVectorSearchPerformsIdLookup() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit}},
    ];

    const mongotResponseBatch =
        [{_id: 2, ...someScore}, {_id: 3, ...someScore}, {_id: 4, ...someScore}];

    const expectedDocs = [
        {_id: 2, x: "now", y: "lorem"},
        {_id: 3, x: "brown", y: "ipsum"},
        {_id: 4, x: "cow", y: "lorem ipsum"}
    ];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// The idLookup stage following $vectorSearch filters orphans, which does not trigger getMores
// (i.e., limit is applied before idLookup).
(function testVectorSearchIdLookupFiltersOrphans() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit: 5}},
    ];

    const mongotResponseBatch = [
        {_id: 3, ...someScore},
        {_id: 4, ...someScore},
        {_id: 5, ...someScore},
        {_id: 6, ...someScore},
        {_id: 7, ...someScore}
    ];

    const expectedDocs = [{_id: 3, x: "brown", y: "ipsum"}, {_id: 4, x: "cow", y: "lorem ipsum"}];

    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery(
            {queryVector, path, numCandidates, limit, collName, dbName, collectionUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];

    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
})();

// Fail on non-local read concern.
(function testVectorSearchFailsOnNonLocalReadConcern() {
    const pipeline = [
        {$vectorSearch: {queryVector, path, numCandidates, limit: 5}},
    ];

    const err = assert.throws(() => coll.aggregate(pipeline, {readConcern: {level: "majority"}}));
    assert.commandFailedWithCode(
        err, [ErrorCodes.InvalidOptions, ErrorCodes.ReadConcernMajorityNotEnabled]);
})();

mongotMock.stop();
MongoRunner.stopMongod(conn);
