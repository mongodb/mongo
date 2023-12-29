/*
 * Test that if a mongod gets an aggregation command from a mongoS with a $search stage it will
 * return two cursors.
 */

import "jstests/libs/sbe_assert_error_override.js";

import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {
    searchShardedExampleCursors1,
    searchShardedExampleCursors2
} from "jstests/with_mongot/search_mocked/lib/search_sharded_example_cursors.js";

const dbName = "test";
const collName = "internal_search_mongot_remote";

const makeInternalConn = (function createInternalClient(conn) {
    const curDB = conn.getDB(dbName);
    assert.commandWorked(curDB.runCommand({
        ["hello"]: 1,
        internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)}
    }));
    return conn;
});

let nodeOptions = {setParameter: {enableTestCommands: 1}};

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
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
const testDb = mongos.getDB(dbName);
const coll = testDb.getCollection(collName);
const collNS = coll.getFullName();

assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
// Shard the test collection.
const splitPoint = 5;
const docList = [];
for (let i = 0; i < 10; i++) {
    docList.push({_id: i, val: i});
}
assert.commandWorked(coll.insert(docList));
st.shardColl(coll, {_id: 1}, {_id: splitPoint}, {_id: splitPoint + 1});

const mongotQuery = {};
const protocolVersion = NumberInt(1);
const shardPipelineStage = {
    "$search": {"mongotQuery": mongotQuery, metadataMergeProtocolVersion: protocolVersion}
};
const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);
// History for shard 1.
{
    const exampleCursor = searchShardedExampleCursors1(
        dbName,
        collNS,
        collName,
        mongotCommandForQuery(mongotQuery, collName, dbName, collUUID0, protocolVersion));
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    s0Mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
    s0Mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
}

// History for shard 2
{
    const exampleCursor = searchShardedExampleCursors2(
        dbName,
        collNS,
        collName,
        mongotCommandForQuery(mongotQuery, collName, dbName, collUUID1, protocolVersion));
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    s1Mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
    s1Mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
}

/**
 * Takes in the response from a query, validates the response, and returns the actual cursor object
 * that has the necessary information to run a getMore.
 */
function validateInitialResponse(thisCursorTopLevel) {
    assert(thisCursorTopLevel.hasOwnProperty("cursor"), thisCursorTopLevel);
    assert(thisCursorTopLevel.hasOwnProperty("ok"), thisCursorTopLevel);
    assert.eq(thisCursorTopLevel.hasOwnProperty("ok"), true, thisCursorTopLevel);
    const thisCursor = thisCursorTopLevel.cursor;
    assert(thisCursor.hasOwnProperty("id"), thisCursor);
    assert(thisCursor.hasOwnProperty("ns"), thisCursor);
    assert(thisCursor.hasOwnProperty("firstBatch"), thisCursor);
    assert(thisCursor.hasOwnProperty("type"), thisCursor);
    return thisCursor;
}

/**
 * Takes in the response from a getMore, validates the fields, and returns the batch for
 * verification.
 */
function validateGetMoreResponse(getMoreRes, expectedId) {
    assert(getMoreRes.hasOwnProperty("cursor"), getMoreRes);
    assert(getMoreRes.hasOwnProperty("ok"), getMoreRes);
    assert.eq(getMoreRes.ok, true, getMoreRes);
    const getMoreCursor = getMoreRes.cursor;
    assert(getMoreCursor.hasOwnProperty("nextBatch"), getMoreCursor);
    assert(getMoreCursor.hasOwnProperty("id"), getMoreCursor);
    assert.eq(getMoreCursor.id, expectedId, getMoreCursor);
    return getMoreCursor.nextBatch;
}
// Run a query against a specific shard to see what a mongod response to a search query looks like.
// Since we are running a pipeline with $_internalSearchMongotRemote we need to use an internal
// client.
const shardZeroConn = makeInternalConn(st.rs0.getPrimary());
const shardZeroDB = shardZeroConn.getDB(dbName);
const shardZeroColl = shardZeroDB[collName];
let commandObj = {
    aggregate: shardZeroColl.getName(),
    pipeline: [shardPipelineStage],
    fromMongos: true,
    needsMerge: true,
    // Internal client has to provide writeConcern
    writeConcern: {w: "majority"},
    // Establishing cursors always has batch size zero.
    cursor: {batchSize: 0},
};
let shardZeroResponse = shardZeroDB.runCommand(commandObj);
// Check that we have a cursors array.
assert(shardZeroResponse.hasOwnProperty("cursors"), shardZeroResponse);
assert(Array.isArray(shardZeroResponse["cursors"]), shardZeroResponse);
let cursorArray = shardZeroResponse.cursors;
assert.eq(cursorArray.length, 2, cursorArray);
for (let thisCursorTopLevel of cursorArray) {
    let thisCursor = validateInitialResponse(thisCursorTopLevel);
    // Iterate the cursor.
    const getMoreRes =
        shardZeroDB.runCommand({getMore: thisCursor.id, collection: shardZeroColl.getName()});

    // Cursor is now exhausted. Verify contents based on type.
    const getMoreResults = validateGetMoreResponse(getMoreRes, 0);
    if (thisCursor.type == "meta") {
        const expectedDocs = [{metaVal: 1}, {metaVal: 2}, {metaVal: 3}, {metaVal: 4}];
        assert.sameMembers(expectedDocs, getMoreResults);
    } else if (thisCursor.type == "results") {
        const expectedDocs = [
            // SortKey and searchScore are included because we're getting results directly from the
            // shard.
            {_id: 1, val: 1, "$searchScore": .4, "$sortKey": [.4]},
            {_id: 2, val: 2, "$searchScore": .3, "$sortKey": [.3]},
            {_id: 3, val: 3, "$searchScore": .123, "$sortKey": [.123]}
        ];
        assert.sameMembers(expectedDocs, getMoreResults);
    } else {
        assert(false, "Unexpected cursor type in \n" + thisCursor);
    }
}

// Repeat for second shard.
// Since we are running a pipeline with $_internalSearchMongotRemote we need to use an internal
// client.
const shardOneConn = makeInternalConn(st.rs1.getPrimary());
const shardOneDB = shardOneConn.getDB(dbName);
const shardOneColl = shardOneDB[collName];
commandObj = {
    aggregate: shardOneColl.getName(),
    pipeline: [shardPipelineStage],
    fromMongos: true,
    needsMerge: true,
    // Internal client requires explicit writeConcern
    writeConcern: {w: "majority"},
    // Establishing cursors always has batch size zero.
    cursor: {batchSize: 0},
};
let shardOneResponse = shardOneDB.runCommand(commandObj);
assert(shardOneResponse.hasOwnProperty("cursors"), shardZeroResponse);
assert(Array.isArray(shardOneResponse["cursors"]), shardZeroResponse);
cursorArray = shardOneResponse.cursors;
assert.eq(cursorArray.length, 2, cursorArray);
for (let thisCursorTopLevel of cursorArray) {
    let thisCursor = validateInitialResponse(thisCursorTopLevel);
    // Iterate the cursor.
    const getMoreRes =
        shardOneDB.runCommand({getMore: thisCursor.id, collection: shardOneColl.getName()});

    // Cursor is now exhausted. Verify contents based on type.
    const getMoreResults = validateGetMoreResponse(getMoreRes, 0);
    if (thisCursor.type == "meta") {
        const expectedDocs = [{metaVal: 10}, {metaVal: 11}, {metaVal: 12}, {metaVal: 13}];
        assert.sameMembers(expectedDocs, getMoreResults);
    } else if (thisCursor.type == "results") {
        // SortKey and searchScore are included because we're getting results directly from the
        // shard.
        const expectedDocs = [
            {_id: 5, val: 5, "$searchScore": .4, "$sortKey": [.4]},
            {_id: 6, val: 6, "$searchScore": .3, "$sortKey": [.3]},
            {_id: 7, val: 7, "$searchScore": .123, "$sortKey": [.123]}
        ];
        assert.sameMembers(expectedDocs, getMoreResults);
    } else {
        assert(false, "Unexpected cursor type in \n" + thisCursor);
    }
}

// Check that if exchange is set on a search query it fails.
commandObj = {
    aggregate: shardOneColl.getName(),
    pipeline: [shardPipelineStage],
    fromMongos: true,
    needsMerge: true,
    exchange: {policy: "roundrobin", consumers: NumberInt(4), bufferSize: NumberInt(1024)},
    writeConcern: {w: "majority"},
    // Establishing cursors always has batch size zero.
    cursor: {batchSize: 0},
};
assert.commandFailedWithCode(shardOneDB.runCommand(commandObj), 6253506);

stWithMock.stop();
