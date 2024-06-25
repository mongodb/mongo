/**
 * Tests for the `$search` aggregation pipeline stage.
 */
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = "test";
const collName = "internal_search_mongot_remote";
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
const collUUID = getUUIDFromListCollections(testDB, collName);

const coll = testDB.getCollection(collName);
coll.insert({_id: 0});

// $search can query mongot and correctly pass along results.
{
    const mongotQuery = {};
    const cursorId = NumberLong(123);
    const pipeline = [{$search: mongotQuery}];
    const mongotResponseBatch = [{_id: 0}];
    const responseOk = 1;
    const expectedDocs = [{_id: 0}];

    const history = [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: collName, db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
}

// $search populates {$meta: searchScore}.
{
    const mongotQuery = {};
    const cursorId = NumberLong(123);
    const pipeline = [{$search: mongotQuery}, {$project: {_id: 1, score: {$meta: "searchScore"}}}];
    const mongotResponseBatch = [{_id: 0, $searchScore: 1.234}];
    const responseOk = 1;
    const expectedDocs = [{_id: 0, score: 1.234}];

    const history = [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: collName, db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
}

// $search populates {$meta: searchScoreDetails}.
{
    const mongotQuery = {scoreDetails: true};
    const cursorId = NumberLong(123);
    const searchScoreDetails = {value: 1.234, description: "great score", details: []};
    const pipeline =
        [{$search: mongotQuery}, {$project: {_id: 1, scoreInfo: {$meta: "searchScoreDetails"}}}];
    const mongotResponseBatch = [{_id: 0, $searchScoreDetails: searchScoreDetails}];
    const responseOk = 1;
    const expectedDocs = [{_id: 0, scoreInfo: searchScoreDetails}];

    const history = [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: collName, db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(testDB[collName].aggregate(pipeline).toArray(), expectedDocs);
}

// mongod fails cleanly when a non-object value is provided to $searchScoreDetails.
{
    const mongotQuery = {scoreDetails: true};
    const cursorId = NumberLong(123);
    const pipeline =
        [{$search: mongotQuery}, {$project: {_id: 1, scoreInfo: {$meta: "searchScoreDetails"}}}];
    const mongotResponseBatch = [{_id: 0, $searchScoreDetails: "great score"}];
    const responseOk = 1;

    const history = [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: collName, db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(mongotResponseBatch, NumberLong(0), collNS, responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    const res = assert.throws(() => testDB[collName].aggregate(pipeline));

    // The aggregate should fail ($searchScoreDetails only accepts objects).
    assert.commandFailedWithCode(res, [10065, 7856603, 8107800]);
}

coll.insert({_id: 1});
coll.insert({_id: 10});
coll.insert({_id: 11});
coll.insert({_id: 20});

// $search handles multiple documents and batches correctly.
{
    const mongotQuery = {};
    const cursorId = NumberLong(123);
    const pipeline = [{$search: mongotQuery}, {$project: {_id: 1, score: {$meta: "searchScore"}}}];

    const batchOne = [{_id: 0, $searchScore: 1.234}, {_id: 1, $searchScore: 1.21}];
    const batchTwo = [{_id: 10, $searchScore: 1.1}, {_id: 11, $searchScore: 0.8}];
    const batchThree = [{_id: 20, $searchScore: 0.2}];
    const expectedDocs = [
        {_id: 0, score: 1.234},
        {_id: 1, score: 1.21},
        {_id: 10, score: 1.1},
        {_id: 11, score: 0.8},
        {_id: 20, score: 0.2},
    ];

    const history = [
        {
            expectedCommand: mongotCommandForQuery(
                {query: mongotQuery, collName: collName, db: dbName, collectionUUID: collUUID}),
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
}

mongotMock.stop();
MongoRunner.stopMongod(conn);
