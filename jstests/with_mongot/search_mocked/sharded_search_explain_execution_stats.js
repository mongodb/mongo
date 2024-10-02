/**
 * Sharding tests for using "explain" with the $search aggregation stage and checks that usage of
 * $$SEARCH_META works as expected. This test checks execution stats are as expected.
 *
 * @tags:[featureFlagSearchExplainExecutionStats]
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
    ShardingTestWithMongotMock,
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {
    getDefaultLastExplainContents,
    getShardedMongotStagesAndValidateExplainExecutionStats,
    setUpMongotReturnExplain,
    setUpMongotReturnExplainAndMultiCursor,
    setUpMongotReturnExplainAndMultiCursorGetMore,
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

assert.commandWorked(coll.insert({_id: 1, name: "Sozin", element: "fire"}));
assert.commandWorked(coll.insert({_id: 2, name: "Zuko", element: "fire"}));
assert.commandWorked(coll.insert({_id: 3, name: "Rangi", element: "fire"}));
assert.commandWorked(coll.insert({_id: 4, name: "Azulon", element: "fire"}));
assert.commandWorked(coll.insert({_id: 11, name: "Iroh", element: "fire"}));
assert.commandWorked(coll.insert({_id: 12, name: "Mako", element: "fire"}));
assert.commandWorked(coll.insert({_id: 13, name: "Azula", element: "fire"}));
assert.commandWorked(coll.insert({_id: 14, name: "Aang", element: "air"}));

// Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
st.shardColl(coll, {_id: 1}, {_id: 10}, {_id: 10 + 1});

const collUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);
const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();

const searchQuery = {
    query: "fire",
    path: "element"
};

let cursorId = NumberLong(123);
const expectedExplainContents = getDefaultLastExplainContents();

function runTestOnPrimaries(testFn) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
}

// Tests $search works with each explain verbosity.
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
            protocolVersion,
        });

        const metaPipeline = [{
            "$group": {
                "_id": {"type": "$type", "path": "$path", "bucket": "$bucket"},
                "value": {
                    "$sum": "$metaVal",
                }
            }
        }];
        const sortSpec = {"$searchSortValues.a": 1};

        const mergingPipelineHistory = [{
            expectedCommand: {
                planShardedSearch: collName,
                query: searchQuery,
                $db: dbName,
                searchFeatures: {shardedSort: 1},
                explain: {verbosity},
            },
            response: {
                ok: 1,
                protocolVersion,
                metaPipeline,
                sortSpec,
            }
        }];
        // TODO SERVER-91594: setUpMongotReturnExplain() can be removed when mongot always
        // returns a cursor alongside explain for execution stats.
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
            // When querying an older version of mongot for explain, the query is sent twice.
            // The second query doesn't include the protocolVersion. This uses a different cursorId
            // than the default one for setUpMongotReturnExplain() so the mock will return the
            // response correctly.
            delete searchCmd.intermediate;
            setUpMongotReturnExplain({
                searchCmd,
                mongotMock: s0Mongot,
                cursorId: NumberLong(124),
            });
            setUpMongotReturnExplain({
                searchCmd,
                mongotMock: s1Mongot,
                cursorId: NumberLong(124),
            });
            searchCmd.intermediate = protocolVersion;

            const result = coll.explain(verbosity).aggregate([{$search: searchQuery}]);
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(0), NumberLong(0)],
                expectedExplainContents,
            });
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchIdLookup",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(0), NumberLong(0)]
            });
            verifyShardsPartExplainOutput(
                {result, searchType: "$search", metaPipeline, protocolVersion, sortSpec});
        }
        {
            stWithMock.getMockConnectedToHost(stWithMock.st.s)
                .setMockResponses(mergingPipelineHistory, cursorId);

            setUpMongotReturnExplainAndMultiCursor({
                searchCmd,
                mongotMock: s0Mongot,
                coll: coll,
                nextBatch: [
                    {_id: 3, $searchScore: 100},
                    {_id: 2, $searchScore: 10},
                    {_id: 4, $searchScore: 1},
                    {_id: 1, $searchScore: 0.99},
                ],
                metaBatch: [{val: 1}]
            });
            setUpMongotReturnExplainAndMultiCursor({
                searchCmd,
                mongotMock: s1Mongot,
                coll: coll,
                nextBatch: [
                    {_id: 11, $searchScore: 100},
                    {_id: 12, $searchScore: 10},
                    {_id: 13, $searchScore: 1},
                ],
                metaBatch: [{val: 2}]
            });

            const result = coll.explain(verbosity).aggregate([{$search: searchQuery}]);
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(4), NumberLong(3)],
                expectedExplainContents,
            });
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchIdLookup",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(4), NumberLong(3)]
            });
            verifyShardsPartExplainOutput(
                {result, searchType: "$search", metaPipeline, protocolVersion, sortSpec});
        }
        {
            stWithMock.getMockConnectedToHost(stWithMock.st.s)
                .setMockResponses(mergingPipelineHistory, cursorId);

            setUpMongotReturnExplainAndMultiCursorGetMore({
                searchCmd,
                mongotMock: s0Mongot,
                coll: coll,
                batchList: [
                    [
                        {_id: 3, $searchScore: 100},
                        {_id: 2, $searchScore: 10},
                    ],
                    [{_id: 4, $searchScore: 1}, {_id: 1, $searchScore: 0.99}],
                ],
                metaBatchList: [[{val: 1}]]
            });
            setUpMongotReturnExplainAndMultiCursorGetMore({
                searchCmd,
                mongotMock: s1Mongot,
                coll: coll,
                batchList: [
                    [{_id: 11, $searchScore: 100}, {_id: 12, $searchScore: 1}],
                    [{_id: 13, $searchScore: 0.99}],
                ],
                metaBatchList: [[{val: 2}]]
            });

            const result = coll.explain(verbosity).aggregate([{$search: searchQuery}],
                                                             {cursor: {batchSize: 2}});
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(4), NumberLong(3)],
                expectedExplainContents,
            });
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchIdLookup",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(4), NumberLong(3)]
            });
            verifyShardsPartExplainOutput(
                {result, searchType: "$search", metaPipeline, protocolVersion, sortSpec});
        }

        // Test demonstrating that $$SEARCH_META after search will not use the metadata
        // pipeline, due to explain not running the merging shard. See SERVER-82206.
        {
            stWithMock.getMockConnectedToHost(stWithMock.st.s)
                .setMockResponses(mergingPipelineHistory, cursorId);

            // Add two extra batches to metaBatchList to confirm that getMore's are not being
            // sent until the cursor is exhuasted. One getMore is for prefetching and one
            // getMore is to confirm that the metadata cursor is not exhausted.
            setUpMongotReturnExplainAndMultiCursorGetMore({
                searchCmd,
                mongotMock: s0Mongot,
                coll: coll,
                batchList: [
                    [
                        {_id: 3, $searchScore: 100},
                        {_id: 2, $searchScore: 10},
                    ],
                    [{_id: 4, $searchScore: 1}, {_id: 1, $searchScore: 0.99}],
                ],
                metaBatchList: [[{val: 1}, {val: 2}], [{val: 3}, {val: 4}], [{val: 6}]],
                metaCursorId: NumberLong(11111),
            });
            setUpMongotReturnExplainAndMultiCursorGetMore({
                searchCmd,
                mongotMock: s1Mongot,
                coll: coll,
                batchList: [
                    [{_id: 11, $searchScore: 100}, {_id: 12, $searchScore: 1}],
                    [{_id: 13, $searchScore: 0.99}],
                ],
                metaBatchList: [[{val: 5}, {val: 6}], [{val: 7}], [{val: 15}]],
                metaCursorId: NumberLong(12345),
            });

            const result = coll.explain(verbosity).aggregate(
                [{$search: searchQuery}, {$project: {_id: 1, meta: "$$SEARCH_META"}}],
                {cursor: {batchSize: 2}});

            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchMongotRemote",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(4), NumberLong(3)],
                expectedExplainContents,
            });
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$_internalSearchIdLookup",
                expectedNumStages: 2,
                verbosity,
                nReturnedList: [NumberLong(4), NumberLong(3)]
            });
            verifyShardsPartExplainOutput(
                {result, searchType: "$search", metaPipeline, protocolVersion, sortSpec});

            // There should be getMore's on mongot that have not been exhausted as the metadata
            // pipeline is not run.
            let errMsg = assert.throws(() => s0Mongot.assertEmpty());
            assert.neq(
                -1, errMsg.message.indexOf("found unused response for cursorID 11111"), errMsg);
            errMsg = assert.throws(() => s1Mongot.assertEmpty());
            assert.neq(
                -1, errMsg.message.indexOf("found unused response for cursorID 12345"), errMsg);

            // Run a getMore on each mock to exhaust the history and reset for the next test.
            assert.commandWorked(s0Mongot.getConnection().getDB("mongotmock").runCommand({
                getMore: NumberLong(11111),
                collection: collName
            }));
            assert.commandWorked(s1Mongot.getConnection().getDB("mongotmock").runCommand({
                getMore: NumberLong(12345),
                collection: collName
            }));

            stWithMock.assertEmptyMocks();
        }
    }
    runTestOnPrimaries(testExplainCase);
    runTestOnSecondaries(testExplainCase);
}

runExplainTest("executionStats");
runExplainTest("allPlansExecution");
stWithMock.stop();
