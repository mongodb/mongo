/**
 * Test that a $search in a view is desugared on each query against the view.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
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

const testDB = st.s.getDB(dbName);
const coll = testDB.getCollection(collName);

// Make shard0 primary shard, but do not shard the collection.
assert.commandWorked(
    st.s.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

assert.commandWorked(coll.insert([
    {_id: 1, a: 10},
    {_id: 2, a: 0},
    {_id: 3, a: 5},
]));

const viewName = collName + "_search_view";

const mongotQuery = {
    text: "foo"
};
assert.commandWorked(testDB.createView(viewName, collName, [{$search: mongotQuery}]));
const searchView = testDB.getCollection(viewName);

let cursorId = 1000;

const mockMongots = function() {
    const s0Mongot = stWithMock.getMockConnectedToHost(st.shard0);
    // Mock responses on primary shard.
    const responseBatch = [
        {_id: 1, $searchScore: 1.0},
        {_id: 2, $searchScore: 0.8},
        {_id: 3, $searchScore: 0.6},
    ];
    const collUUID = getUUIDFromListCollections(testDB, collName);
    s0Mongot.setMockResponses(
        [{
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID,
            }),
            response: mongotResponseForBatch(responseBatch, NumberLong(0), coll.getFullName(), 1),
        }],
        cursorId);
    cursorId++;
};

// Ensure that $search is desugared twice for two queries against the view. Each desugaring phase
// will cause the primary shard to invoke planShardedSearch.
mockMongots();
assert.eq([{_id: 1, a: 10}], searchView.find().limit(1).toArray());
mockMongots();
assert.eq([{_id: 1, a: 10}], searchView.find().limit(1).toArray());

// Helper function which gets the pipeline associated with a view defintion.
function getPipelineFromViewDef(viewName) {
    return testDB.system.views.findOne({_id: viewName}).pipeline;
}

// Check explicitly that the $search pipeline was not desugared in the view.
assert.eq([{$search: mongotQuery}], getPipelineFromViewDef(searchView.getFullName()));

// Ensure that $searchMeta does not desugar in view definition parsing.
{
    const searchMetaViewName = collName + "_search_meta_view";
    assert.commandWorked(
        testDB.createView(searchMetaViewName, collName, [{$searchMeta: mongotQuery}]));
    const searchMetaView = testDB.getCollection(searchMetaViewName);
    assert.eq([{$searchMeta: mongotQuery}], getPipelineFromViewDef(searchMetaView.getFullName()));
}

stWithMock.stop();
