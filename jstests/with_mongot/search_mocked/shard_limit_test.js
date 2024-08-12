/**
 * Test that a $limit gets pushed to the shards.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mockPlanShardedSearchResponse,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = "internal_search_mongot_remote";

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
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
const testColl = testDB.getCollection(collName);
let protocolVersion = null;

// TODO SERVER-85637 Remove check for SearchExplainExecutionStats after the feature flag is removed.
if (FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchExplainExecutionStats') &&
    !(checkSbeRestrictedOrFullyEnabled(testDB) &&
      FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe'))) {
    protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();
}

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

let mongotQuery = {};
const explainVerbosity = "executionStats";
let expectedMongotCommand = {
    search: "internal_search_mongot_remote",
    collectionUUID: collUUID0,
    query: mongotQuery,
    explain: {verbosity: explainVerbosity},
    $db: "test"
};
if (protocolVersion != null) {
    expectedMongotCommand.intermediate = protocolVersion;
}
let cursorId = NumberLong(123);
let pipeline = [
    {$search: mongotQuery},
    // Skip should be considered part of the limit for how many documents to get from shards.
    {$skip: 5},
    {$limit: 2},
];

function runTestOnPrimaries(testFn, cursorId) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary(), cursorId);
}

function assertLimitAbsorbed(explainRes, query) {
    let shardsArray = explainRes["shards"];

    for (let [_, individualShardObj] of Object.entries(shardsArray)) {
        // TODO: See SERVER-84511 Explain seems to be broken with search. Currently, "stages" is not
        // found in the bsonObj in the loop below.
        if (individualShardObj["stages"] === undefined) {
            continue;
        }
        let stages = individualShardObj["stages"];
        // Assert limit was pushed down to shards.
        if ("returnStoredSource" in query) {
            assert.eq(stages[0]["$_internalSearchMongotRemote"].limit, 7, explainRes);
        } else {
            assert.eq(stages[1]["$_internalSearchIdLookup"].limit, 7, explainRes);
        }
        // Assert limit and skip were pushed down to mongot in the form of 'mongotRequestedDocs'.
        // Both need to be pushed down so that after mongos skips first documents in sort order, the
        // limit can then be applied.
        assert.eq(7, stages[0]["$_internalSearchMongotRemote"].mongotDocsRequested, explainRes);
    }
}

function testBasicCase(shard0Conn, shard1Conn, cursorId) {
    const history = [{
        expectedCommand: expectedMongotCommand,
        response:
            mongotResponseForBatch([], NumberLong(0), testColl.getFullName(), 1, {"garbage": true})
    }];
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    s0Mongot.setMockResponses(history, cursorId);

    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    s1Mongot.setMockResponses(history, cursorId);

    mockPlanShardedSearchResponse(testColl.getName(),
                                  mongotQuery,
                                  dbName,
                                  undefined /*sortSpec*/,
                                  stWithMock,
                                  false /*maybeUnused*/,
                                  {verbosity: explainVerbosity});
    const explainResult = testColl.explain(explainVerbosity).aggregate(pipeline);
    assertLimitAbsorbed(explainResult, mongotQuery);
}
runTestOnPrimaries(testBasicCase, NumberLong(123));

// Run again with storedSource.
mongotQuery = {
    returnStoredSource: true
};
pipeline = [
    {$search: mongotQuery},
    // Skip should be considered part of the limit for how many documents to get from shards.
    {$skip: 5},
    {$limit: 2},
];
expectedMongotCommand = {
    search: "internal_search_mongot_remote",
    collectionUUID: collUUID0,
    query: mongotQuery,
    explain: {verbosity: explainVerbosity},
    $db: "test"
};
if (protocolVersion != null) {
    expectedMongotCommand.intermediate = protocolVersion;
}
runTestOnPrimaries(testBasicCase, NumberLong(124));
stWithMock.stop();
