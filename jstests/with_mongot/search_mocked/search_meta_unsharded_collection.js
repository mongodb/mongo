/**
 * Test that $searchMeta works correctly on an unsharded collection through mongos.
 */
import {hangTestToAttachGDB} from "jstests/libs/hang_test_to_attach_gdb.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {
    expectedSearchMeta,
    expectPlanShardedSearch,
    searchQuery
} from "jstests/with_mongot/search_mocked/lib/server_85694_query_constants.js";

let nodeOptions = {
    setParameter:
        {enableTestCommands: 1, logComponentVerbosity: tojson({query: 5, command: 2, network: 0})}
};

const stWithMock = new ShardingTestWithMongotMock({
    name: "search_meta_unsharded",
    shards: {rs0: {nodes: 1}},
    config: 1,
    mongos: 1,
    other: {rsOptions: nodeOptions, mongosOptions: nodeOptions}
});

stWithMock.start();
const st = stWithMock.st;

const conn = stWithMock.st.s;
const mongotForTheMongos = stWithMock.getMockConnectedToHost(conn);

const dbName = "test";
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const collName = jsTestName();
const coll = testDB.getCollection(collName);

const singleResultId = ObjectId("65ba75afca88f584bdbac735");

assert.commandWorked(coll.insertOne(
    {_id: singleResultId, openfda: {manufacturer_name: 'Factory', route: ['ORAL']}}));

// Set the mock responses for a query which includes the result cursors.
function setQueryMockResponses(isSearchMeta) {
    expectPlanShardedSearch({mongotConn: mongotForTheMongos, coll: coll});
    const mongotQuery = searchQuery;
    const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
    const shard0MongoT = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());

    const throwawayCursorID = NumberLong(1);  // Mongot wants a cursor ID even if we don't use it.
    shard0MongoT.setMockResponses(
        [
            {
                // Please note: 'protocolVersion' is intentionally left off. There is no merging
                // pipeline here, so we expect full results - not intermediate results - from the
                // mongot.
                expectedCommand: mongotCommandForQuery({
                    query: mongotQuery,
                    collName: collName,
                    db: dbName,
                    collectionUUID: collUUID0
                }),
                response: {
                    "cursor": {
                        "id": NumberLong(0),
                        "nextBatch": [{"_id": singleResultId, "$searchScore": 1.0}],
                        "ns": coll.getFullName(),
                    },
                    "vars": {"SEARCH_META": expectedSearchMeta},
                    "ok": 1
                }
            },
        ],
        throwawayCursorID);
}

// Test that a $search query properly computes the $$SEARCH_META value according to the pipeline
// returned by mongot(mock).
function testSearchQuery() {
    setQueryMockResponses();
    let queryResult =
        coll.aggregate([{$search: searchQuery}, {$project: {meta: "$$SEARCH_META"}}]).toArray();
    assert.eq([{_id: singleResultId, meta: expectedSearchMeta}], queryResult);
}

// Test that a $searchMeta query properly computes the metadata value according to the pipeline
// returned by mongot(mock).
function testSearchMetaQuery() {
    setQueryMockResponses();
    let queryResult = coll.aggregate([{$searchMeta: searchQuery}]);
    // Same as above query result but not embedded in a document.
    assert.eq([expectedSearchMeta], queryResult.toArray());
}

testSearchMetaQuery();
testSearchQuery();

stWithMock.stop();
