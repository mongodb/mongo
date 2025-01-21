/**
 * Test that $searchMeta works correctly on a sharded collection when used inside a sub-pipeline,
 * like within a $unionWith and a $lookup.
 *
 * This test is an adaptation of an end-to-end test written on the master branch, backported to 8.0.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

function runTest() {
    const defaultOpts = {
        name: "search_meta_in_subpipeline_sharded",
        shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
        config: 1,
        mongos: 1,
    };
    const stWithMock = new ShardingTestWithMongotMock(defaultOpts);

    stWithMock.start();
    const st = stWithMock.st;

    const conn = stWithMock.st.s;
    const mongotForTheMongos = stWithMock.getMockConnectedToHost(conn);
    function onEachShardPrimarysMongot(callback) {
        callback(stWithMock.getMockConnectedToHost(st.rs0.getPrimary()));
        callback(stWithMock.getMockConnectedToHost(st.rs1.getPrimary()));
    }

    // Disable order checking.
    onEachShardPrimarysMongot(
        mongot => assert.commandWorked(
            mongot.getConnection().getDB("test").runCommand({setOrderCheck: false})));

    const dbName = "test";
    const testDB = conn.getDB(dbName);
    assert.commandWorked(
        testDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

    const collName = jsTestName();
    const coll = testDB.getCollection(collName);

    const shard0ResultId = -1;
    const shard1ResultId = 1;

    const baseCollDocs = [
        {_id: shard0ResultId, element: "fire", index: 101, genre: "Drama"},
        {_id: shard1ResultId, element: "fire", index: 900, genre: "Comedy"}
    ];
    assert.commandWorked(coll.insertMany(baseCollDocs));
    // Shard the collection by '_id', splitting at 0 and moving the [0, MaxKey] chunk to shard1.
    st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});

    const countQuery = {
        index: "facet-index",
        range: {path: "index", gte: 100, lt: 9000},
        count: {type: "total"}
    };
    const searchMetaQuery = {$searchMeta: countQuery};

    const mockedMetaFromShard0 = [{type: "count", tag: 1, bucket: 2, count: NumberLong(1)}];
    const mockedMetaFromShard1 = mockedMetaFromShard0;

    const expectedMergingPipeline = [
        {$group: {_id: {type: "$type", tag: "$tag", bucket: "$bucket"}, value: {$sum: "$count"}}},
        {$facet: {count: [{$match: {"_id.type": {$eq: "count"}}}]}},
        {$replaceRoot: {newRoot: {count: {total: {$first: ["$count.value"]}}}}}
    ];
    const expectedSearchMeta = [{count: {total: NumberLong(2)}}];

    const protocolVersion = NumberInt(1);
    let nextCursorId = 1;
    let resultsCursorId = NumberLong(100);
    let metaCursorId = NumberLong(1000);
    function expectPlanShardedSearch({mongotConn, coll}) {
        const cmds = [{
            expectedCommand: {
                planShardedSearch: coll.getName(),
                query: countQuery,
                $db: coll.getDB().getName(),
                searchFeatures: {shardedSort: 1}
            },
            response: {
                ok: 1,
                protocolVersion: protocolVersion,
                metaPipeline: expectedMergingPipeline,
                sortSpec: {$searchScore: -1},
            }
        }];
        mongotConn.setMockResponses(cmds, nextCursorId++);
    }
    // Set the mock responses for a query which includes the result cursors.
    function setQueryMockResponses() {
        const mongotQuery = countQuery;
        function mockForShard({shardPrimary, mockedId, mockedMeta}) {
            const collUUID = getUUIDFromListCollections(shardPrimary.getDB(dbName), collName);
            const mongot = stWithMock.getMockConnectedToHost(shardPrimary);
            mongot.setMockResponses(
                [
                    {
                        // Please note: intermediate results protocol version is not expected.
                        expectedCommand: mongotCommandForQuery({
                            query: mongotQuery,
                            collName: collName,
                            db: dbName,
                            collectionUUID: collUUID,
                            protocolVersion: protocolVersion
                        }),
                        response:
                            mongotMultiCursorResponseForBatch([{_id: mockedId, $searchScore: 1.0}],
                                                              NumberLong(0),
                                                              mockedMeta,
                                                              NumberLong(0),
                                                              coll.getFullName(),
                                                              1)
                    },
                ],
                resultsCursorId++,
                metaCursorId++);
        }
        mockForShard({
            shardPrimary: st.rs0.getPrimary(),
            mockedId: shard0ResultId,
            mockedMeta: mockedMetaFromShard0
        });
        mockForShard({
            shardPrimary: st.rs1.getPrimary(),
            mockedId: shard1ResultId,
            mockedMeta: mockedMetaFromShard1
        });
    }

    // Test a 'normal' $searchMeta query.
    function testSearchMetaQuery() {
        expectPlanShardedSearch({mongotConn: mongotForTheMongos, coll: coll});
        setQueryMockResponses();
        let queryResult = coll.aggregate([searchMetaQuery]);
        // Same as above query result but not embedded in a document.
        assert.eq(expectedSearchMeta, queryResult.toArray());
    }

    // Test $searchMeta within a $unionWith.
    function testUnionWithSearchMeta() {
        expectPlanShardedSearch({mongotConn: mongotForTheMongos, coll: coll});
        setQueryMockResponses();
        let queryResult =
            coll.aggregate([{$unionWith: {coll: coll.getName(), pipeline: [searchMetaQuery]}}]);
        // Same as above query result but not embedded in a document.
        assert.sameMembers(baseCollDocs.concat(expectedSearchMeta), queryResult.toArray());
    }

    function repeat({fn, nTimes}) {
        for (let i = 0; i < nTimes; ++i) {
            fn();
        }
    }

    // Test $searchMeta within a $lookup.
    function testLookupWithSearchMeta() {
        expectPlanShardedSearch({mongotConn: mongotForTheMongos, coll: coll});
        onEachShardPrimarysMongot(mongot => {
            repeat({
                fn: () => expectPlanShardedSearch({mongotConn: mongot, coll: coll}),
                nTimes: baseCollDocs.length
            });
        });
        repeat({fn: setQueryMockResponses, nTimes: baseCollDocs.length});
        let queryResult = coll.aggregate(
            [{$lookup: {from: coll.getName(), pipeline: [searchMetaQuery], as: "metaLookup"}}]);
        assert.sameMembers(baseCollDocs.map(doc => ({...doc, metaLookup: expectedSearchMeta})),
                           queryResult.toArray());
    }

    testSearchMetaQuery();
    stWithMock.assertEmptyMocks();

    testUnionWithSearchMeta();
    stWithMock.assertEmptyMocks();

    testLookupWithSearchMeta();
    stWithMock.assertEmptyMocks();

    stWithMock.stop();
}
runTest();
