/**
 * Sharding test for the `$vectorSearch` aggregation pipeline stage. This test uses a somewhat
 * random data set.
 * @tags: [
 *   requires_fcv_71,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = "sharded_vector_search_random_dataset";

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_vector_search",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1,
    other: {
        rsOptions: {setParameter: {enableTestCommands: 1}},
    }
});
stWithMock.start();
const st = stWithMock.st;
const mongos = st.s;
const testDb = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDb.getCollection(collName);
const collNS = testColl.getFullName();

Random.setRandomSeed();

const docIdToScore = {};
let shard0Ids = [];
let shard1Ids = [];
const splitPoint = 100;
const docsToInsert = [];
for (let i = 0; i < 200; ++i) {
    const score = Random.rand();
    docIdToScore[i] = score;
    docsToInsert.push({_id: i, unusedValue: "hello world"});

    if (i < 100) {
        shard0Ids.push(i);
    } else {
        shard1Ids.push(i);
    }
}
assert.commandWorked(testColl.insert(docsToInsert));

// Compare two values for _id based on their score.
function scoreComparator(idA, idB) {
    return docIdToScore[idA] < docIdToScore[idB];
}

shard0Ids.sort(scoreComparator);
shard1Ids.sort(scoreComparator);

// Shard the test collection, split it, and move the higher chunk to shard1.
st.shardColl(testColl, {_id: 1}, {_id: splitPoint}, {_id: splitPoint + 1});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 200
};
const cursorId = NumberLong(123);
const metaCursorId = NumberLong(0);
const pipeline = [
    {$vectorSearch: vectorSearchQuery},
];

// Given an array of ids and a range, create an array of the form:
// [{_id: <first id in array>, $vectorSearchScore: <score for _id>}, ...].
function constructMongotResponseBatchForIds(ids, startIdx, endIdx) {
    const batch = [];
    for (let i = startIdx; i < endIdx; ++i) {
        batch.push({_id: ids[i], $vectorSearchScore: docIdToScore[ids[i]]});
    }
    return batch;
}

const responseOk = 1;

const expectedMongotCommand = mongotCommandForVectorSearchQuery(
    {...vectorSearchQuery, collName, dbName, collectionUUID: collUUID0});

// Set up history for the mock associated with the primary of shard 0.
{
    const history = [
        {
            expectedCommand: expectedMongotCommand,
            response: mongotResponseForBatch(
                constructMongotResponseBatchForIds(shard0Ids, 0, 30), cursorId, collNS, responseOk),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(constructMongotResponseBatchForIds(shard0Ids, 30, 60),
                                             cursorId,
                                             collNS,
                                             responseOk),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(
                constructMongotResponseBatchForIds(shard0Ids, 60, shard0Ids.length),
                NumberLong(0),
                collNS,
                responseOk)
        }
    ];
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    s0Mongot.setMockResponses(history, cursorId);
}

// Set up history for the mock associated with the primary of shard 1.
{
    const history = [
        {
            expectedCommand: expectedMongotCommand,
            response: mongotResponseForBatch(
                constructMongotResponseBatchForIds(shard1Ids, 0, 30), cursorId, collNS, responseOk),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(constructMongotResponseBatchForIds(shard1Ids, 30, 70),
                                             cursorId,
                                             collNS,
                                             responseOk),
        },
        {
            expectedCommand: {getMore: cursorId, collection: collName},
            response: mongotResponseForBatch(
                constructMongotResponseBatchForIds(shard1Ids, 70, shard1Ids.length),
                NumberLong(0),
                collNS,
                responseOk)
        }
    ];
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    s1Mongot.setMockResponses(history, cursorId);
}

// Be sure the vectorSearchScore results are in descending order.
const queryResults = testColl.aggregate(pipeline).toArray();
assert.eq(queryResults.length, shard0Ids.length + shard1Ids.length);

let maxSearchScoreSeen = docIdToScore[queryResults[0]._id];
for (let result of queryResults) {
    const newSearchScore = docIdToScore[result._id];
    assert.lte(newSearchScore, maxSearchScoreSeen, {queryResults, docIdToScore});
    maxSearchScoreSeen = newSearchScore;
}

stWithMock.stop();
