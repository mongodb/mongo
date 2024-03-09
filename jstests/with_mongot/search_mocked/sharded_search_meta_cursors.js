/*
 * Test that if a mongod gets an aggregation command from a mongoS with a $search stage it will
 * return two cursors by default or when requiresSearchMetaCursor is explicitly true, and will
 * only return one cursor when requiresSearchMetaCursor is explicitly false.
 */

import "jstests/libs/sbe_assert_error_override.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
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

// TODO SERVER-87335 Make this work for SBE
if (checkSbeRestrictedOrFullyEnabled(testDb) &&
    FeatureFlagUtil.isPresentAndEnabled(testDb.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    stWithMock.stop();
    quit();
}

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

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

function mockShardZero(metaCursorWillBeKilled = false) {
    const exampleCursor = searchShardedExampleCursors1(
        dbName,
        collNS,
        collName,
        mongotCommandForQuery(mongotQuery, collName, dbName, collUUID0, protocolVersion));
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());
    // The getMore will be pre-fetched, but if the meta cursor is deemed unnecessary, we'll issue a
    // killCursors on the meta cursor. If the connection is pinned, we'll try to cancel the getMore
    // as we killCursors, so the getMore may go unused. (When the connection isn't pinned, we don't
    // try to cancel the in-flight getMore).
    if (metaCursorWillBeKilled) {
        exampleCursor.historyMeta[0].maybeUnused = true;
    }
    s0Mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
    s0Mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
}

function mockShardOne(metaCursorWillBeKilled = false) {
    const exampleCursor = searchShardedExampleCursors2(
        dbName,
        collNS,
        collName,
        mongotCommandForQuery(mongotQuery, collName, dbName, collUUID1, protocolVersion));
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());
    // See comment in mockShardZero().
    if (metaCursorWillBeKilled) {
        exampleCursor.historyMeta[0].maybeUnused = true;
    }
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

// Command obj to be sent to mongod. The "pipeline" field will be configured per test.
let commandObj = {
    aggregate: collName,
    fromMongos: true,
    needsMerge: true,
    // Internal client has to provide writeConcern
    writeConcern: {w: "majority"},
    // Establishing cursors always has batch size zero.
    cursor: {batchSize: 0},
};
const shardPipelineRequiresMetaCursorImplicit = {
    "$search": {"mongotQuery": mongotQuery, metadataMergeProtocolVersion: protocolVersion}
};
const shardPipelineRequiresMetaCursorExplicit = {
    "$search": {
        "mongotQuery": mongotQuery,
        metadataMergeProtocolVersion: protocolVersion,
        requiresSearchMetaCursor: true
    }
};
const shardPipelineDoesntRequireMetaCursor = {
    "$search": {
        "mongotQuery": mongotQuery,
        metadataMergeProtocolVersion: protocolVersion,
        requiresSearchMetaCursor: false
    }
};

/**
 * Tests that mongod returns 2 cursors (a results cursor and a meta cursor) for the given search
 * stage. Below, we run this once with a stage that explicitly sets requiresSearchMetaCursor to
 * true, and once with a stage where it's implicitly true by omitting the option.
 */
function runTestRequiresMetaCursorOnConn(shardDB, searchStage, expectedDocs, expectedMetaResults) {
    commandObj.pipeline = [searchStage];
    const shardResponse = shardDB.runCommand(commandObj);
    // Check that we have a cursors array.
    assert(shardResponse.hasOwnProperty("cursors"), shardResponse);
    assert(Array.isArray(shardResponse["cursors"]), shardResponse);
    let cursorArray = shardResponse.cursors;
    assert.eq(cursorArray.length, 2, cursorArray);
    for (let thisCursorTopLevel of cursorArray) {
        let thisCursor = validateInitialResponse(thisCursorTopLevel);
        // Since we have two cursors, each cursor should have a specified "type".
        assert(thisCursor.hasOwnProperty("type"), thisCursorTopLevel);
        // Iterate the cursor.
        const getMoreRes = shardDB.runCommand({getMore: thisCursor.id, collection: collName});

        // Cursor is now exhausted. Verify contents based on type.
        const getMoreResults = validateGetMoreResponse(getMoreRes, 0);
        if (thisCursor.type == "meta") {
            assert.sameMembers(expectedMetaResults, getMoreResults);
        } else if (thisCursor.type == "results") {
            assert.sameMembers(expectedDocs, getMoreResults);
        } else {
            assert(false, "Unexpected cursor type in \n" + thisCursor);
        }
    }
}

/**
 * Tests that mongod returns just the results cursor when requiresSearchMetaCursor is explicitly set
 * to false.
 */
function runTestNoMetaCursorOnConn(shardDB, expectedDocs) {
    // Run the command that explicitly says no meta cursor is needed.
    commandObj.pipeline = [shardPipelineDoesntRequireMetaCursor];
    const shardResponse = shardDB.runCommand(commandObj);
    let cursor = validateInitialResponse(shardResponse);

    // Iterate the cursor.
    const getMoreRes = shardDB.runCommand({getMore: cursor.id, collection: collName});

    // Cursor is now exhausted.
    const getMoreResults = validateGetMoreResponse(getMoreRes, 0);
    assert.sameMembers(expectedDocs, getMoreResults);
}

// Run queries against a specific shard to see what a mongod response to a search query looks like.
// Since we are running a pipeline with $_internalSearchMongotRemote we need to use an internal
// client.
let expectedDocs = [
    // SortKey and searchScore are included because we're getting results directly from the
    // shard.
    {_id: 1, val: 1, "$searchScore": .4, "$sortKey": [.4]},
    {_id: 2, val: 2, "$searchScore": .3, "$sortKey": [.3]},
    {_id: 3, val: 3, "$searchScore": .123, "$sortKey": [.123]}
];
let expectedMetaResults = [{metaVal: 1}, {metaVal: 2}, {metaVal: 3}, {metaVal: 4}];
const shardZeroConn = makeInternalConn(st.rs0.getPrimary());
const shardZeroDB = shardZeroConn.getDB(dbName);
mockShardZero();
runTestRequiresMetaCursorOnConn(
    shardZeroDB, shardPipelineRequiresMetaCursorImplicit, expectedDocs, expectedMetaResults);
mockShardZero();
runTestRequiresMetaCursorOnConn(
    shardZeroDB, shardPipelineRequiresMetaCursorExplicit, expectedDocs, expectedMetaResults);
mockShardZero(/*metaCursorWillBeKilled*/ true);
runTestNoMetaCursorOnConn(shardZeroDB, expectedDocs);

// Repeat for second shard.
expectedDocs = [
    {_id: 5, val: 5, "$searchScore": .4, "$sortKey": [.4]},
    {_id: 6, val: 6, "$searchScore": .3, "$sortKey": [.3]},
    {_id: 7, val: 7, "$searchScore": .123, "$sortKey": [.123]}
];
expectedMetaResults = [{metaVal: 10}, {metaVal: 11}, {metaVal: 12}, {metaVal: 13}];
const shardOneConn = makeInternalConn(st.rs1.getPrimary());
const shardOneDB = shardOneConn.getDB(dbName);
mockShardOne();
runTestRequiresMetaCursorOnConn(
    shardOneDB, shardPipelineRequiresMetaCursorImplicit, expectedDocs, expectedMetaResults);
mockShardOne();
runTestRequiresMetaCursorOnConn(
    shardOneDB, shardPipelineRequiresMetaCursorExplicit, expectedDocs, expectedMetaResults);
mockShardOne(/*metaCursorWillBeKilled*/ true);
runTestNoMetaCursorOnConn(shardOneDB, expectedDocs);

// Check that if exchange is set on a search query it fails.
commandObj = {
    aggregate: collName,
    pipeline: [shardPipelineRequiresMetaCursorImplicit],
    fromMongos: true,
    needsMerge: true,
    exchange: {policy: "roundrobin", consumers: NumberInt(4), bufferSize: NumberInt(1024)},
    writeConcern: {w: "majority"},
    // Establishing cursors always has batch size zero.
    cursor: {batchSize: 0},
};
assert.commandFailedWithCode(shardOneDB.runCommand(commandObj), 6253506);

stWithMock.stop();
