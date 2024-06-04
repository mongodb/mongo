/**
 * Tests that a query with an extractable limit prevents mongod from prefetching multiple batches
 * from mongot.
 * @tags: [requires_fcv_71]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestricted} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    mongotKillCursorResponse,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: "limit_prefetch",
    shards: {
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    mongos: 1
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

// Skip the test if running in 'trySbeRestricted' mode with 'SearchInSbe' enabled. In this mode,
// $search will be pushed down to SBE, but $limit will not.
if (checkSbeRestricted(testDB) &&
    FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    stWithMock.stop();
    quit();
}

if (FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchBatchSizeTuning')) {
    jsTestLog("Skipping the test because it only applies when batchSize isn't enabled.");
    stWithMock.stop();
    quit();
}

const testColl = testDB.getCollection(collName);

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

assert.commandWorked(testColl.insert({_id: 1, x: "ow"}));
assert.commandWorked(testColl.insert({_id: 2, x: "now", y: "lorem"}));
assert.commandWorked(testColl.insert({_id: 3, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 4, x: "cow", y: "lorem ipsum"}));
assert.commandWorked(testColl.insert({_id: 11, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 12, x: "cow", y: "lorem ipsum"}));
assert.commandWorked(testColl.insert({_id: 13, x: "brown", y: "ipsum"}));
assert.commandWorked(testColl.insert({_id: 14, x: "cow", y: "lorem ipsum"}));

const collUUID0 = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

const shard0Conn = st.rs0.getPrimary();
const shard1Conn = st.rs1.getPrimary();

function mockShards(mongotQuery, shard0Docs, shard1Docs) {
    // Return batches with an open cursorId to have mongod think there are more results behind the
    // cursor. Because there is an extractable limit, the server should disable prefetching from
    // mongot. Since the returned batch satifies the limit, the server will not send a getmore for
    // this cursor and eventually will kill the cursor when the query is done.
    const mongotCursorId = NumberLong(123);
    const metaId = NumberLong(2);
    const history0 = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID0,
                protocolVersion: protocolVersion,
                cursorOptions: {docsRequested: 2}
            }),
            response: mongotMultiCursorResponseForBatch(shard0Docs,
                                                        // Return non-closed cursorId.
                                                        mongotCursorId,
                                                        [{metaVal: 1}],
                                                        // Return closed meta cursorId.
                                                        NumberLong(0),
                                                        testColl.getFullName(),
                                                        NumberLong(1)),
        },
        mongotKillCursorResponse(collName, mongotCursorId),
    ];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history0, mongotCursorId, metaId);
    const history1 = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: collName,
                db: dbName,
                collectionUUID: collUUID0,
                protocolVersion: protocolVersion,
                cursorOptions: {docsRequested: 2}
            }),
            response: mongotMultiCursorResponseForBatch(shard1Docs,
                                                        // Return non-closed cursorId.
                                                        mongotCursorId,
                                                        [{metaVal: 1}],
                                                        // Return closed meta cursorId.
                                                        NumberLong(0),
                                                        testColl.getFullName(),
                                                        NumberLong(1) /*ok*/),
        },
        mongotKillCursorResponse(collName, mongotCursorId),
    ];
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history1, mongotCursorId, metaId);
}

(function testBasicCase() {
    const mongotQuery = {query: "lorem"};
    const pipeline = [
        {$search: mongotQuery},
        {$limit: 2},
    ];
    mockPlanShardedSearchResponse(
        testColl.getName(), mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);
    mockShards(
        mongotQuery,
        [{_id: 1, $searchScore: 0.5}, {_id: 2, $searchScore: 0.3}],
        [{_id: 11, $searchScore: 0.4}, {_id: 12, $searchScore: 0.2}],
    );
    assert.eq(
        [
            {_id: 1, x: "ow"},
            {_id: 11, x: "brown", y: "ipsum"},
        ],
        testColl.aggregate(pipeline).toArray(),
    );
})();

(function testLimitStoredSource() {
    const mongotQuery = {query: "lorem", returnStoredSource: true};
    const pipeline = [
        {$search: mongotQuery},
        {$limit: 2},
    ];
    mockPlanShardedSearchResponse(
        testColl.getName(), mongotQuery, dbName, undefined /*sortSpec*/, stWithMock);
    mockShards(
        mongotQuery,
        [
            {storedSource: {_id: 1, x: "ow"}, $searchScore: 0.5},
            {storedSource: {_id: 2, x: "now"}, $searchScore: 0.3},
        ],
        [
            {storedSource: {_id: 11, x: "brown"}, $searchScore: 0.4},
            {storedSource: {_id: 12, x: "cow"}, $searchScore: 0.2},
        ],
    );
    assert.eq(
        [
            {_id: 1, x: "ow"},
            {_id: 11, x: "brown"},
        ],
        testColl.aggregate(pipeline).toArray(),
    );
})();

stWithMock.stop();
