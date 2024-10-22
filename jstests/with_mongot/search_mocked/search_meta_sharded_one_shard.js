/**
 * Test that $searchMeta works correctly on an sharded collection but when there is only one chunk
 * for the collection - on a single shard.
 */
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
    setParameter: {enableTestCommands: 1, logComponentVerbosity: tojson({query: 0})}
};

function runTest(customStOpts) {
    const defaultOpts = {
        name: "search_meta_unsharded",
        shards: {rs0: {nodes: 1}},
        config: 1,
        mongos: 1,
        other: {rsOptions: nodeOptions, mongosOptions: nodeOptions}
    };
    const stWithMock = new ShardingTestWithMongotMock({...defaultOpts, ...customStOpts});

    stWithMock.start();
    const st = stWithMock.st;

    const conn = stWithMock.st.s;
    const mongotForTheMongos = stWithMock.getMockConnectedToHost(conn);

    const dbName = "test";
    const testDB = conn.getDB(dbName);
    assert.commandWorked(
        testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    const collName = jsTestName();
    const coll = testDB.getCollection(collName);

    const shard0ResultId = 2;

    assert.commandWorked(coll.insert([
        {_id: shard0ResultId, openfda: {manufacturer_name: 'Factory', route: ['ORAL']}},
    ]));
    // Shard the collection by '_id', but do not split it or move any chunks off the primary shard
    // (shard 0).
    st.shardColl(coll, {_id: 1}, false, false);

    // Set the mock responses for a query which includes the result cursors.
    function setQueryMockResponses() {
        expectPlanShardedSearch({mongotConn: mongotForTheMongos, coll: coll});
        const mongotQuery = searchQuery;
        {
            const collUUID0 =
                getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
            const mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
            const resultsId = NumberLong(2);
            mongot.setMockResponses(
                [
                    {
                        // Please note: intermediate results protocol version is not expected.
                        expectedCommand: mongotCommandForQuery({
                            query: mongotQuery,
                            collName: collName,
                            db: dbName,
                            collectionUUID: collUUID0
                        }),
                        response: {
                            "cursor": {
                                "id": NumberLong(0),
                                "nextBatch": [{"_id": shard0ResultId, "$searchScore": 1.0}],
                                "ns": coll.getFullName(),
                            },
                            "vars": {"SEARCH_META": expectedSearchMeta},
                            "ok": 1
                        }
                    },
                ],
                resultsId);
        }
    }

    // Test that a $search query properly computes the $$SEARCH_META value according to the pipeline
    // returned by mongot(mock).
    function testSearchQuery() {
        setQueryMockResponses();
        let queryResult =
            coll.aggregate([{$search: searchQuery}, {$project: {"var": "$$SEARCH_META"}}])
                .toArray();
        assert.eq([{_id: shard0ResultId, var : expectedSearchMeta}], queryResult);
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
}
// To be exhaustive, cover the case where there is only one shard in the whole cluster, vs. multiple
// shards available but the collection resides entirely on one of them.
runTest({shards: {rs0: {nodes: 1}}});
runTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}});
