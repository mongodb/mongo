/**
 * Sharding tests for the `$meta: "searchSequenceToken"`expression that can be in a stage, here a
 * $project, following a search query .
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search_sequence_token",
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
    other: {
        rsOptions: {setParameter: {enableTestCommands: 1}},
    }
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testColl = testDB.getCollection(collName);
const collNS = testColl.getFullName();
testColl.drop();

assert.commandWorked(testColl.insert({_id: 1}));
assert.commandWorked(testColl.insert({_id: 2}));
assert.commandWorked(testColl.insert({_id: 3}));
assert.commandWorked(testColl.insert({_id: 4}));
assert.commandWorked(testColl.insert({_id: 11}));
assert.commandWorked(testColl.insert({_id: 12}));
assert.commandWorked(testColl.insert({_id: 13}));
assert.commandWorked(testColl.insert({_id: 14}));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

const mongotQuery = {};
const protocolVersion = NumberInt(1);
const cursorOptions = {
    requiresSearchSequenceToken: true
};
const expectedMongotCommand =
    mongotCommandForQuery(mongotQuery, collName, dbName, collUUID0, protocolVersion, cursorOptions);

const cursorId = NumberLong(123);
const secondCursorId = NumberLong(cursorId + 1001);

function runTestOnPrimaries(testFn) {
    const owningShardMerge = {"specificShard": st.shard0.shardName};
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary(), owningShardMerge, true);
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary(), owningShardMerge, false);
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary(), "anyShard", true);
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary(), "anyShard", false);
}

function testBasicCase(shard0Conn, shard1Conn, mergeType, splitPipelineBeforeProject) {
    const responseOk = 1;
    let pipeline = [];
    // $_internalSplitPipeline forces the stages before it in the pipeline to run on the shards and
    // the stages after it to run on mongos. We need to guarantee that regardless of splitting the
    // pipeline before or after $project (which is how we find out that the search sequence token is
    // required), that mongot winds up generating the tokens correctly.
    if (splitPipelineBeforeProject) {
        pipeline = [
            {$search: mongotQuery},
            {$_internalSplitPipeline: {"mergeType": mergeType}},
            {$project: {myToken: {$meta: "searchSequenceToken"}}}
        ];
    } else {
        pipeline = [
            {$search: mongotQuery},
            {$project: {myToken: {$meta: "searchSequenceToken"}}},
            {$_internalSplitPipeline: {"mergeType": mergeType}},
            {$addFields: {"newField": 1}}
        ];
    }
    const mongot0ResponseBatch = [
        {_id: 3, $searchScore: 100, $searchSequenceToken: "bbbbbbb=="},
        {_id: 2, $searchScore: 10, $searchSequenceToken: "fffffff=="},
        {_id: 4, $searchScore: 1, $searchSequenceToken: "ggggggg=="},
        {_id: 1, $searchScore: 0.99, $searchSequenceToken: "hhhhhhh=="},
    ];
    const history0 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotMultiCursorResponseForBatch(
            mongot0ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
    }];

    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, cursorId, secondCursorId);

    const mongot1ResponseBatch = [
        {_id: 11, $searchScore: 111, $searchSequenceToken: "aaaaaaa=="},
        {_id: 13, $searchScore: 30, $searchSequenceToken: "ccccccc=="},
        {_id: 12, $searchScore: 29, $searchSequenceToken: "ddddddd=="},
        {_id: 14, $searchScore: 28, $searchSequenceToken: "eeeeeee=="},
    ];
    const history1 = [{
        expectedCommand: expectedMongotCommand,
        response: mongotMultiCursorResponseForBatch(
            mongot1ResponseBatch, NumberLong(0), [{val: 1}], NumberLong(0), collNS, responseOk),
    }];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, cursorId, secondCursorId);

    let expectedDocs = [
        {_id: 11, myToken: "aaaaaaa=="},
        {_id: 3, myToken: "bbbbbbb=="},
        {_id: 13, myToken: "ccccccc=="},
        {_id: 12, myToken: "ddddddd=="},
        {_id: 14, myToken: "eeeeeee=="},
        {_id: 2, myToken: "fffffff=="},
        {_id: 4, myToken: "ggggggg=="},
        {_id: 1, myToken: "hhhhhhh=="},
    ];

    if (!splitPipelineBeforeProject) {
        expectedDocs.map((obj) => {
            obj["newField"] = 1;
        });
    }
    mockPlanShardedSearchResponse(
        testColl.getName(), mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);

    assert.eq(testColl.aggregate(pipeline).toArray(), expectedDocs);
}

runTestOnPrimaries(testBasicCase);
runTestOnSecondaries(testBasicCase);

stWithMock.stop();
