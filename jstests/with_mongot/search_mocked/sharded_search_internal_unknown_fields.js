/*
 * Test that if a mongos sends mongod a $search pipeline with an unknown field in the $search stage,
 * mongod will ignore the superfluous field.
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */

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

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const collUUID1 = getUUIDFromListCollections(st.rs1.getPrimary().getDB(dbName), collName);

function mockShardZero() {
    const exampleCursor =
        searchShardedExampleCursors1(dbName, collNS, collName, mongotCommandForQuery({
                                         query: mongotQuery,
                                         collName: collName,
                                         db: dbName,
                                         collectionUUID: collUUID0,
                                         protocolVersion: protocolVersion
                                     }));
    const s0Mongot = stWithMock.getMockConnectedToHost(st.rs0.getPrimary());

    s0Mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
    s0Mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
}

function mockShardOne() {
    const exampleCursor =
        searchShardedExampleCursors2(dbName, collNS, collName, mongotCommandForQuery({
                                         query: mongotQuery,
                                         collName: collName,
                                         db: dbName,
                                         collectionUUID: collUUID1,
                                         protocolVersion: protocolVersion
                                     }));
    const s1Mongot = stWithMock.getMockConnectedToHost(st.rs1.getPrimary());

    s1Mongot.setMockResponses(exampleCursor.historyResults, exampleCursor.resultsID);
    s1Mongot.setMockResponses(exampleCursor.historyMeta, exampleCursor.metaID);
}

// Build the command obj to be sent to mongod.
const shardPipelineWithUnknownFields = {
    "$search": {
        "mongotQuery": mongotQuery,
        metadataMergeProtocolVersion: protocolVersion,
        unknownField: "cake carrots apple kale",
    }
};

const commandObj = {
    aggregate: collName,
    fromRouter: true,
    needsMerge: true,
    // Internal client has to provide writeConcern
    writeConcern: {w: "majority"},
    // Establishing cursors always has batch size zero.
    cursor: {batchSize: 0},
    pipeline: [shardPipelineWithUnknownFields],
};

/**
 * Tests that mongod returns the expected results cursor and meta cursor when there is a superfluous
 * field in the $search aggregation stage.
 */
function runAndAssertMongodIgnoresUnknownFieldInSearch(shardDB, expectedDocs, expectedMetaResults) {
    const shardResponse = shardDB.runCommand(commandObj);
    let cursorArray = shardResponse.cursors;

    for (let thisCursorTopLevel of cursorArray) {
        let thisCursor = thisCursorTopLevel.cursor;
        const getMoreRes = assert.commandWorked(
            shardDB.runCommand({getMore: thisCursor.id, collection: collName}));

        // Verify the results cursor's contents.
        const getMoreResults = getMoreRes.cursor.nextBatch;
        if (thisCursor.type == "meta") {
            assert.sameMembers(expectedMetaResults, getMoreResults);
        } else if (thisCursor.type == "results") {
            assert.sameMembers(expectedDocs, getMoreResults);
        } else {
            assert(false, "Unexpected cursor type in \n" + thisCursor);
        }
    }
}

// Run queries against a specific shard to see what a mongod response to a search query looks like.
const expectedDocsOnShardZero = [
    // SortKey and searchScore are included because we're getting results directly from the
    // shard.
    {_id: 1, val: 1, "$searchScore": .4, "$score": .4, "$sortKey": [.4]},
    {_id: 2, val: 2, "$searchScore": .3, "$score": .3, "$sortKey": [.3]},
    {_id: 3, val: 3, "$searchScore": .123, "$score": .123, "$sortKey": [.123]}
];
const expectedMetaResultsOnShardZero = [{metaVal: 1}, {metaVal: 2}, {metaVal: 3}, {metaVal: 4}];

// Since we are simulating a $search pipeline sent from the router, we need to use an internal
// client.
const shardZeroConn = makeInternalConn(st.rs0.getPrimary());
const shardZeroDB = shardZeroConn.getDB(dbName);
mockShardZero();
runAndAssertMongodIgnoresUnknownFieldInSearch(
    shardZeroDB, expectedDocsOnShardZero, expectedMetaResultsOnShardZero);

// Repeat for the second shard.
const expectedDocsOnShardOne = [
    {_id: 5, val: 5, "$searchScore": .4, "$score": .4, "$sortKey": [.4]},
    {_id: 6, val: 6, "$searchScore": .3, "$score": .3, "$sortKey": [.3]},
    {_id: 7, val: 7, "$searchScore": .123, "$score": .123, "$sortKey": [.123]}
];
const expectedMetaResultsOnShardOne = [{metaVal: 10}, {metaVal: 11}, {metaVal: 12}, {metaVal: 13}];
const shardOneConn = makeInternalConn(st.rs1.getPrimary());
const shardOneDB = shardOneConn.getDB(dbName);
mockShardOne();
runAndAssertMongodIgnoresUnknownFieldInSearch(
    shardOneDB, expectedDocsOnShardOne, expectedMetaResultsOnShardOne);

stWithMock.stop();
