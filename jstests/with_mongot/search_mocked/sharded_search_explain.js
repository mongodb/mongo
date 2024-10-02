/**
 * Sharding tests for using "explain" with the $search aggregation stage for queries that do not
 * contain meaningful executionStats. This tests "queryPlanner" verbosity (no execution stats). It
 * also tests  "executionStats" and "allPlansExecution" without the feature flag for explain
 * execution stats enabled.
 *
 * @tags:[featureFlagSearchExplainExecutionStats_incompatible]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {verifyShardsPartExplainOutput} from "jstests/with_mongot/common_utils.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mongotCommandForQuery,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {
    getDefaultLastExplainContents,
    getShardedMongotStagesAndValidateExplainExecutionStats,
    setUpMongotReturnExplain,
    setUpMongotReturnExplainAndCursor,
} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = jsTestName();
const collName = "search";

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
});
stWithMock.start();
const st = stWithMock.st;

const mongos = st.s;
const testDB = mongos.getDB(dbName);
if (checkSbeRestrictedOrFullyEnabled(testDB) &&
    FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe')) {
    jsTestLog("Skipping the test because it only applies to $search in classic engine.");
    stWithMock.stop();
    quit();
}

assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const coll = testDB.getCollection(collName);

assert.commandWorked(coll.insert({_id: 1, name: "Sokka"}));
assert.commandWorked(coll.insert({_id: 2, name: "Zuko", element: "fire"}));
assert.commandWorked(coll.insert({_id: 3, name: "Katara", element: "water"}));
assert.commandWorked(coll.insert({_id: 4, name: "Toph", element: "earth"}));
assert.commandWorked(coll.insert({_id: 11, name: "Aang", element: "air"}));
assert.commandWorked(coll.insert({_id: 12, name: "Ty Lee"}));
assert.commandWorked(coll.insert({_id: 13, name: "Azula", element: "fire"}));
assert.commandWorked(coll.insert({_id: 14, name: "Iroh", element: "fire"}));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(coll, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

const searchQuery = {
    query: "fire",
    path: "element"
};

const expectedExplainContents = getDefaultLastExplainContents();

const cursorId = NumberLong(123);
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

function runTestOnPrimaries(testFn) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
}

function runExplainTest(verbosity) {
    function testExplainCase(shard0Conn, shard1Conn) {
        // sX is shard num X.
        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
        // Ensure there is never a staleShardVersionException to cause a retry on any shard.
        // If a retry happens on one shard and not another, then the shard that did not retry
        // will see multiple instances of the explain command, which the test does not expect,
        // causing an error.
        st.refreshCatalogCacheForNs(mongos, coll.getFullName());

        const searchCmd = mongotCommandForQuery({
            query: searchQuery,
            collName: collName,
            db: dbName,
            collectionUUID: collUUID,
            explainVerbosity: {verbosity},
        });
        const metaPipeline = [{
            "$group": {
                "_id": {"type": "$type", "path": "$path", "bucket": "$bucket"},
                "value": {
                    "$sum": "$metaVal",
                }
            }
        }];

        const mergingPipelineHistory = [{
            expectedCommand: {
                planShardedSearch: collName,
                query: searchQuery,
                $db: dbName,
                searchFeatures: {shardedSort: 1},
                explain: {verbosity}
            },
            response: {
                ok: 1,
                protocolVersion,
                metaPipeline,
            }
        }];
        // TODO SERVER-91594: Testing "executionStats" and "allPlansExecution" with
        // setUpMongotReturnExplain() is not necessary after mongot will always return a cursor for
        // execution stats verbosities.
        {
            stWithMock.getMockConnectedToHost(stWithMock.st.s)
                .setMockResponses(mergingPipelineHistory, cursorId);

            setUpMongotReturnExplain({
                searchCmd,
                mongotMock: s0Mongot,
            });
            setUpMongotReturnExplain({
                searchCmd,
                mongotMock: s1Mongot,
            });

            const result = coll.explain(verbosity).aggregate([{$search: searchQuery}]);

            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(0), NumberLong(0)],
                expectedExplainContents,
            });
            verifyShardsPartExplainOutput(
                {result, searchType: "$search", metaPipeline, protocolVersion});
        }

        // TODO SERVER-85637 Remove the following block of code with
        // setUpMongotReturnExplainAndMultiCursor(), as it will fail with the feature flag
        // removed. This scenario is already tested with execution stats enabled in
        // sharded_search_explain_execution_stats.js.
        if (verbosity != "queryPlanner") {
            {
                stWithMock.getMockConnectedToHost(stWithMock.st.s)
                    .setMockResponses(mergingPipelineHistory, cursorId);

                // With the feature flag disabled, the protocolVersion will not be included, so only
                // an explain and cursor object will be returned.
                setUpMongotReturnExplainAndCursor({
                    mongotMock: s0Mongot,
                    coll,
                    searchCmd,
                    nextBatch: [
                        {_id: 1, $searchScore: 0.321},
                    ],
                });
                setUpMongotReturnExplainAndCursor({
                    mongotMock: s1Mongot,
                    coll,
                    searchCmd,
                    nextBatch: [
                        {_id: 1, $searchScore: 0.321},
                    ],
                });

                const result = coll.explain(verbosity).aggregate([{$search: searchQuery}]);
                getShardedMongotStagesAndValidateExplainExecutionStats({
                    result,
                    stageType: "$_internalSearchMongotRemote",
                    expectedNumStages: 2,
                    verbosity,
                    nReturnedList: [NumberLong(0), NumberLong(0)],
                    expectedExplainContents,
                });
                verifyShardsPartExplainOutput(
                    {result, searchType: "$search", metaPipeline, protocolVersion});
            }
        }
    }
    runTestOnPrimaries(testExplainCase);
    runTestOnSecondaries(testExplainCase);
}

runExplainTest("queryPlanner");
runExplainTest("executionStats");
runExplainTest("allPlansExecution");

stWithMock.stop();
