/**
 * Sharding tests that cover a variety of different possible distributed execution scenarios.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = "internal_search_mongot_remote";

let nodeOptions = {setParameter: {enableTestCommands: 1}};
const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
    other: {
        rsOptions: nodeOptions,
        mongosOptions: nodeOptions,
        shardOptions: nodeOptions,
    }
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

function setupCollection(localName) {
    const testColl = testDB.getCollection(localName);

    assert.commandWorked(testColl.insert({_id: 1, x: "ow", val: 1}));
    assert.commandWorked(testColl.insert({_id: 2, x: "now", y: "lorem", val: 2}));
    assert.commandWorked(testColl.insert({_id: 3, x: "brown", y: "ipsum", val: 3}));
    assert.commandWorked(testColl.insert({_id: 4, x: "cow", y: "lorem ipsum", val: 4}));
    assert.commandWorked(testColl.insert({_id: 11, x: "brown", y: "ipsum", val: 111}));
    assert.commandWorked(testColl.insert({_id: 12, x: "cow", y: "lorem ipsum", val: 112}));
    assert.commandWorked(testColl.insert({_id: 13, x: "brown", y: "ipsum", val: 113}));
    assert.commandWorked(testColl.insert({_id: 14, x: "cow", y: "lorem ipsum", val: 114}));

    // Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
    st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

    return testColl;
}
const mongotQuery = {};
const cursorId = NumberLong(123);
let testColl = setupCollection(collName);
// View queries resolve to the base namespace, so always use this.
const collNS = testColl.getFullName();
const collUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), testColl.getName());
const protocolVersion = NumberLong(42);

function setUpMerge(mergeType, localColl, isView) {
    const pipeline = [
        {$search: mongotQuery},
        {$_internalSplitPipeline: {"mergeType": mergeType}},
        {$project: {_id: 1, meta: "$$SEARCH_META.arr"}},
    ];
    // A view already has the search stage
    if (isView) {
        pipeline.shift();
    }
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 100},
        {_id: 2, $searchScore: 10},
        {_id: 4, $searchScore: 1},
        {_id: 1, $searchScore: 0.99},
    ];
    const mongot0Response =
        mongotMultiCursorResponseForBatch(mongot0ResponseBatch,
                                          NumberLong(0),
                                          [{type: "a", val: 7}, {type: "b", val: 4}],
                                          NumberLong(0),
                                          collNS,
                                          responseOk);
    const history0 = [{
        expectedCommand: mongotCommandForQuery(
            mongotQuery, testColl.getName(), dbName, collUUID, protocolVersion),
        response: mongot0Response
    }];

    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 111},
        {_id: 13, $searchScore: 30},
        {_id: 12, $searchScore: 29},
        {_id: 14, $searchScore: 28},
    ];
    const mongot1Response =
        mongotMultiCursorResponseForBatch(mongot1ResponseBatch,
                                          NumberLong(0),
                                          [{type: "a", val: 12}, {type: "b", val: 10}],
                                          NumberLong(0),
                                          collNS,
                                          responseOk);
    const history1 = [{
        expectedCommand: mongotCommandForQuery(
            mongotQuery, testColl.getName(), dbName, collUUID, protocolVersion),
        response: mongot1Response
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    s0Mongot.setMockResponses(history0, cursorId, NumberLong(20));
    s1Mongot.setMockResponses(history1, cursorId, NumberLong(20));

    const mergingPipelineHistory = [{
        expectedCommand: {
            planShardedSearch: testColl.getName(),
            query: mongotQuery,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: NumberInt(42),
            metaPipeline: [
                {
                    "$group": {
                        "_id": {
                            "type": "$type",
                        },
                        "val": {
                            "$sum": "$val",
                        }
                    }
                },
                {$project: {_id: 0, type: "$_id.type", count: "$val"}},
                {$sort: {"type": 1}},
                {
                    "$group": {
                        "_id": null,
                        "arr": {
                            $push: "$$ROOT",
                        }
                    }
                }
            ]
        }
    }];
    const mongot = stWithMock.getMockConnectedToHost(stWithMock.st.s);
    mongot.setMockResponses(mergingPipelineHistory, 1);
    return pipeline;
}

function testMergeAtLocation(mergeType, localColl, isView) {
    const metaDoc = [{type: "a", count: 19}, {type: "b", count: 14}];
    const expectedDocs = [
        {_id: 11, meta: metaDoc},
        {_id: 3, meta: metaDoc},
        {_id: 13, meta: metaDoc},
        {_id: 12, meta: metaDoc},
        {_id: 14, meta: metaDoc},
        {_id: 2, meta: metaDoc},
        {_id: 4, meta: metaDoc},
        {_id: 1, meta: metaDoc},
    ];

    const pipeline = setUpMerge(mergeType, localColl, isView);
    assert.eq(localColl.aggregate(pipeline).toArray(), expectedDocs);
}

function testSearchMetaFailure(mergeType, localColl, isView) {
    setUpMerge(mergeType, localColl, isView);
    const pipeline = setUpMerge(mergeType, localColl, isView);

    assert.commandFailedWithCode(
        localColl.runCommand({aggregate: localColl.getName(), pipeline: pipeline, cursor: {}}),
        6347902);
}

function testMergeAtLocationSearchMeta(mergeType, localColl, isView) {
    const pipeline = [
        {$searchMeta: mongotQuery},
        {$_internalSplitPipeline: {"mergeType": mergeType}},
        {$project: {_id: 0}},
    ];
    // A view already has the search stage
    if (isView) {
        pipeline.shift();
    }
    const responseOk = 1;

    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 100},
        {_id: 2, $searchScore: 10},
        {_id: 4, $searchScore: 1},
        {_id: 1, $searchScore: 0.99},
    ];
    const mongot0Response =
        mongotMultiCursorResponseForBatch(mongot0ResponseBatch,
                                          NumberLong(0),
                                          [{type: "a", val: 7}, {type: "b", val: 4}],
                                          NumberLong(0),
                                          collNS,
                                          responseOk);
    const history0 = [{
        expectedCommand: mongotCommandForQuery(
            mongotQuery, testColl.getName(), dbName, collUUID, protocolVersion),
        response: mongot0Response
    }];

    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 111},
        {_id: 13, $searchScore: 30},
        {_id: 12, $searchScore: 29},
        {_id: 14, $searchScore: 28},
    ];
    const mongot1Response =
        mongotMultiCursorResponseForBatch(mongot1ResponseBatch,
                                          NumberLong(0),
                                          [{type: "a", val: 12}, {type: "b", val: 10}],
                                          NumberLong(0),
                                          collNS,
                                          responseOk);
    const history1 = [{
        expectedCommand: mongotCommandForQuery(
            mongotQuery, testColl.getName(), dbName, collUUID, protocolVersion),
        response: mongot1Response
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    s0Mongot.setMockResponses(history0, cursorId, NumberLong(20));
    s1Mongot.setMockResponses(history1, cursorId, NumberLong(20));

    const metaDoc = [{type: "a", count: 19}, {type: "b", count: 14}];
    const expectedDocs = [{arr: metaDoc}];

    const mergingPipelineHistory = [{
        expectedCommand: {
            planShardedSearch: testColl.getName(),
            query: mongotQuery,
            $db: dbName,
            searchFeatures: {shardedSort: 1}
        },
        response: {
            ok: 1,
            protocolVersion: NumberInt(42),
            metaPipeline: [
                {
                    "$group": {
                        "_id": {
                            "type": "$type",
                        },
                        "val": {
                            "$sum": "$val",
                        }
                    }
                },
                {$project: {_id: 0, type: "$_id.type", count: "$val"}},
                {$sort: {"type": 1}},
                {
                    "$group": {
                        "_id": null,
                        "arr": {
                            $push: "$$ROOT",
                        }
                    }
                }
            ]
        }
    }];
    const mongot = stWithMock.getMockConnectedToHost(stWithMock.st.s);
    mongot.setMockResponses(mergingPipelineHistory, 1);
    assert.eq(localColl.aggregate(pipeline).toArray(), expectedDocs);
}

const owningShardMerge = {
    "specificShard": st.shard0.shardName
};
testMergeAtLocation("mongos", testColl, false);
testMergeAtLocation("anyShard", testColl, false);
testMergeAtLocation(owningShardMerge, testColl, false);
testMergeAtLocation("localOnly", testColl, false);

testMergeAtLocationSearchMeta("mongos", testColl, false);
testMergeAtLocationSearchMeta("anyShard", testColl, false);
testMergeAtLocationSearchMeta(owningShardMerge, testColl, false);
// Repeat, but the collection is a view.
assert.commandWorked(
    testDB.createView(collName + "viewColl", testColl.getName(), [{$search: mongotQuery}], {}));
let viewColl = testDB.getCollection(collName + "viewColl");
testMergeAtLocation("mongos", viewColl, true);
testMergeAtLocation("anyShard", viewColl, true);
testMergeAtLocation(owningShardMerge, viewColl, true);
testMergeAtLocation("localOnly", viewColl, true);

assert(viewColl.drop());

// Create a view that does not use $search. Verify that we can detect an invalid use of
// $$SEARCH_META.
assert.commandWorked(testDB.createView(
    collName + "viewColl", testColl.getName(), [{$match: {_id: {$gt: -1000}}}], {}));
viewColl = testDB.getCollection(collName + "viewColl");
testSearchMetaFailure("mongos", viewColl, true);
testSearchMetaFailure("anyShard", viewColl, true);
testSearchMetaFailure(owningShardMerge, viewColl, true);
testSearchMetaFailure("localOnly", viewColl, true);

assert(viewColl.drop());

assert.commandWorked(
    testDB.createView(collName + "viewColl", testColl.getName(), [{$searchMeta: mongotQuery}], {}));
viewColl = testDB.getCollection(collName + "viewColl");
testMergeAtLocationSearchMeta("mongos", testColl, false);
testMergeAtLocationSearchMeta("anyShard", viewColl, true);
testMergeAtLocationSearchMeta(owningShardMerge, viewColl, true);
stWithMock.stop();
