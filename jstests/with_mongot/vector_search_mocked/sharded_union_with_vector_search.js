/**
 * Testing sharded $unionWith: {$vectorSearch}
 */

import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const shardedVectorSearchCollName = "vector_search_sharded";
const unshardedVectorSearchCollName = "vector_search_unsharded";
const shardedBaseCollName = "base_sharded";
const unshardedBaseCollName = "base_unsharded";

// Start mock mongot.
const stWithMock = new ShardingTestWithMongotMock({
    name: jsTestName(),
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1
});
stWithMock.start();
const st = stWithMock.st;

// Start mongod.
const mongos = st.s;
const testDB = mongos.getDB(dbName);
assertCreateCollection(testDB, shardedVectorSearchCollName);
assertCreateCollection(testDB, shardedBaseCollName);
assertCreateCollection(testDB, unshardedVectorSearchCollName);
assertCreateCollection(testDB, unshardedBaseCollName);

const vectorSearchCollDocs = [{_id: 1, title: "Tarzan the Ape Man"}, {_id: 2, title: "King Kong"}];
const shardedVectorSearchColl = testDB.getCollection(shardedVectorSearchCollName);
shardedVectorSearchColl.insert(vectorSearchCollDocs);
const unshardedVectorSearchColl = testDB.getCollection(unshardedVectorSearchCollName);
unshardedVectorSearchColl.insert(vectorSearchCollDocs);

const baseCollDocs = [
    {_id: 100, localField: "cakes", weird: false},
    {_id: 101, localField: "cakes and kale", weird: true}
];
const shardedBaseColl = testDB.getCollection(shardedBaseCollName);
shardedBaseColl.insert(baseCollDocs);
const unshardedBaseColl = testDB.getCollection(unshardedBaseCollName);
unshardedBaseColl.insert(baseCollDocs);

const shardedVectorSearchCollectionUUID =
    getUUIDFromListCollections(testDB, shardedVectorSearchCollName);
const shardedVectorSearchCollNS = dbName + "." + shardedVectorSearchCollName;

const unshardedVectorSearchCollectionUUID =
    getUUIDFromListCollections(testDB, unshardedVectorSearchCollName);
const unshardedVectorSearchCollNS = dbName + "." + unshardedVectorSearchCollName;

// Shard vector search collection.
st.shardColl(shardedVectorSearchColl, {_id: 1}, {_id: 2}, {_id: 2});
// Shard base collection.
st.shardColl(shardedBaseColl, {_id: 1}, {_id: 101}, {_id: 101});

const shard0Conn = st.rs0.getPrimary();
const shard1Conn = st.rs1.getPrimary();
const d0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
const d1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const limit = 5;

const cursorId = NumberLong(123);
const responseOk = 1;

function setupVectorSearchResponse(isSharded, mongotConn, mongotResponseBatch) {
    const history = [{
        expectedCommand: mongotCommandForVectorSearchQuery({
            queryVector,
            path,
            limit,
            collName: isSharded ? shardedVectorSearchCollName : unshardedVectorSearchCollName,
            dbName,
            collectionUUID: isSharded ? shardedVectorSearchCollectionUUID
                                      : unshardedVectorSearchCollectionUUID
        }),
        response: mongotResponseForBatch(
            mongotResponseBatch,
            NumberLong(0),
            isSharded ? shardedVectorSearchCollNS : unshardedVectorSearchCollNS,
            responseOk),
    }];
    mongotConn.setMockResponses(history, cursorId);
}
function setupShard0VectorSearchResponse() {
    const mongotResponseBatch = [{_id: 2, $vectorSearchScore: 0.8}];
    setupVectorSearchResponse(true /* isSharded */, d0Mongot, mongotResponseBatch);
}
function setupShard1VectorSearchResponse() {
    const mongotResponseBatch = [{_id: 1, $vectorSearchScore: 1}];
    setupVectorSearchResponse(true /* isSharded */, d1Mongot, mongotResponseBatch);
}
function setupUnshardedVectorSearchResponses() {
    const mongotResponseBatch =
        [{_id: 1, $vectorSearchScore: 1}, {_id: 2, $vectorSearchScore: 0.8}];
    setupVectorSearchResponse(false /* isSharded */, d1Mongot, mongotResponseBatch);
}

const makeUnionWithPipeline = (vectorSearchColl) => [{
    $unionWith: {
        coll: vectorSearchColl.getName(),
        pipeline: [
            {
                $vectorSearch: {
                    queryVector,
                    path,
                    limit,
                }
            },
            {$project: {_id: 0, title: 1, score: {$meta: 'vectorSearchScore'}}}
        ]
    }
}];

const expectedUnionWithResult = baseCollDocs.concat(
    [{title: "Tarzan the Ape Man", score: 1}, {title: "King Kong", score: 0.8}]);

function unionTest(baseColl, vectorSearchColl, vectorSearchCollSharded) {
    if (vectorSearchCollSharded) {
        setupShard0VectorSearchResponse();
        setupShard1VectorSearchResponse();
    } else {
        setupUnshardedVectorSearchResponses();
    }

    assert.sameMembers(baseColl.aggregate(makeUnionWithPipeline(vectorSearchColl)).toArray(),
                       expectedUnionWithResult);
    stWithMock.assertEmptyMocks();
}

// Test all combinations of sharded/unsharded base/vector search collection.
unionTest(unshardedBaseColl, unshardedVectorSearchColl, false);

unionTest(unshardedBaseColl, shardedVectorSearchColl, true);

unionTest(shardedBaseColl, unshardedVectorSearchColl, false);

unionTest(shardedBaseColl, shardedVectorSearchColl, true);

stWithMock.stop();
