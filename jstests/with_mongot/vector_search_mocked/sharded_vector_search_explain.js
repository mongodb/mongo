/**
 * Sharding tests for using "explain" with the $vectorSearch aggregation stage.
 * @tags: [
 *  featureFlagVectorSearchPublicPreview,
 * ]
 */
import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForVectorSearchQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {
    getDefaultLastExplainContents,
    getShardedMongotStagesAndValidateExplainExecutionStats,
    setUpMongotReturnExplain,
    setUpMongotReturnExplainAndCursor,
    setUpMongotReturnExplainAndCursorGetMore,
} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = jsTestName();
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: jsTestName(),
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

const collectionUUID = getUUIDFromListCollections(st.rs0.getPrimary().getDB(dbName), collName);

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 100,
    limit: 10,
    index: "idx",
    filter: {x: {$gt: 1}},
};

const lowerUserLimit = vectorSearchQuery.limit - 1;
const higherUserLimit = vectorSearchQuery.limit + 1;

const expectedExplainContents = getDefaultLastExplainContents();

function runTestOnPrimaries(testFn) {
    testDB.getMongo().setReadPref("primary");
    testFn(st.rs0.getPrimary(), st.rs1.getPrimary());
}

function runTestOnSecondaries(testFn) {
    testDB.getMongo().setReadPref("secondary");
    testFn(st.rs0.getSecondary(), st.rs1.getSecondary());
}

function testLimit(shard0Conn, shard1Conn, verbosity, userLimit) {
    // sX is shard num X.
    const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
    const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
    // Ensure there is never a staleShardVersionException to cause a retry on any shard.
    // If a retry happens on one shard and not another, then the shard that did not retry
    // will see multiple instances of the explain command, which the test does not expect,
    // causing an error.
    st.refreshCatalogCacheForNs(mongos, coll.getFullName());

    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        ...vectorSearchQuery,
        explain: {verbosity},
        collName,
        dbName,
        collectionUUID,
    });

    if (verbosity === "queryPlanner") {
        setUpMongotReturnExplain({
            searchCmd: vectorSearchCmd,
            mongotMock: s0Mongot,
        });
        setUpMongotReturnExplain({
            searchCmd: vectorSearchCmd,
            mongotMock: s1Mongot,
        });
    } else {
        setUpMongotReturnExplainAndCursor({
            mongotMock: s0Mongot,
            coll,
            searchCmd: vectorSearchCmd,
            nextBatch: [
                {_id: 3, $vectorSearchScore: 100},
                {_id: 2, $vectorSearchScore: 10},
                {_id: 4, $vectorSearchScore: 1},
                {_id: 1, $vectorSearchScore: 0.2},
            ],
        });
        setUpMongotReturnExplainAndCursor({
            mongotMock: s1Mongot,
            coll,
            searchCmd: vectorSearchCmd,
            nextBatch: [
                {_id: 11, $vectorSearchScore: 100},
                {_id: 12, $vectorSearchScore: 10},
                {_id: 13, $vectorSearchScore: 1},
            ],
        });
    }
    const result = coll.explain(verbosity).aggregate(
        [{$vectorSearch: vectorSearchQuery}, {$limit: userLimit}]);
    // We should have a $limit on each shard.
    const limitStages = getAggPlanStages(result, "$limit");
    assert.eq(limitStages.length, 2, tojson(result));
    // The $limits will take on the value of the $vectorSearch limit unless we have a smaller
    // user-specified $limit.
    const expectedLimitVal =
        userLimit ? Math.min(userLimit, vectorSearchQuery.limit) : vectorSearchQuery.limit;
    for (const limitStage of limitStages) {
        assert.eq(expectedLimitVal, limitStage["$limit"], tojson(result));
    }
    // The merging pipeline should also have a $limit with the minimum of the $vectorSearch limit
    // and user-specified $limit.
    assert(result.splitPipeline.mergerPart[0].hasOwnProperty("$mergeCursors"),
           tojson(result.splitPipeline));
    assert(result.splitPipeline.mergerPart[1].hasOwnProperty("$limit"),
           tojson(result.splitPipeline));
    assert.eq(
        result.splitPipeline.mergerPart[1].$limit, expectedLimitVal, tojson(result.splitPipeline));
}
function runExplainTest(verbosity) {
    // Ensure there is never a staleShardVersionException to cause a retry on any shard.
    // If a retry happens on one shard and not another, then the shard that did not retry
    // will see multiple instances of the explain command, which the test does not expect,
    // causing an error.
    st.refreshCatalogCacheForNs(mongos, coll.getFullName());

    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        ...vectorSearchQuery,
        explain: {verbosity},
        collName,
        dbName,
        collectionUUID,
    });
    const pipeline = [{$vectorSearch: vectorSearchQuery}];

    function runExplainQueryPlannerTest(shard0Conn, shard1Conn) {
        assert.eq(verbosity, "queryPlanner");
        // sX is shard num X.
        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);
        {
            setUpMongotReturnExplain({
                searchCmd: vectorSearchCmd,
                mongotMock: s0Mongot,
            });
            setUpMongotReturnExplain({
                searchCmd: vectorSearchCmd,
                mongotMock: s1Mongot,
            });
            const result = coll.explain(verbosity).aggregate(pipeline);

            getShardedMongotStagesAndValidateExplainExecutionStats({
                result: result,
                stageType: "$vectorSearch",
                verbosity: verbosity,
                expectedNumStages: 2,
                nReturnedList: [NumberLong(0), NumberLong(0)],
                expectedExplainContents: expectedExplainContents,
            });
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result: result,
                stageType: "$_internalSearchIdLookup",
                expectedNumStages: 2,
                nReturnedList: [NumberLong(0), NumberLong(0)],
                verbosity: verbosity,
            });
        }
    }
    function runExplainExecutionStatsTest(shard0Conn, shard1Conn) {
        assert.neq(verbosity, "queryPlanner");
        // sX is shard num X.
        const s0Mongot = stWithMock.getMockConnectedToHost(shard0Conn);
        const s1Mongot = stWithMock.getMockConnectedToHost(shard1Conn);

        // TODO SERVER-91594: setUpMongotReturnExplain() can be removed when mongot always
        // returns a cursor alongside explain for execution stats.
        {
            setUpMongotReturnExplain({
                searchCmd: vectorSearchCmd,
                mongotMock: s0Mongot,
            });
            setUpMongotReturnExplain({
                searchCmd: vectorSearchCmd,
                mongotMock: s1Mongot,
            });
            // When querying an older version of mongot for explain, the query is sent twice. This
            // uses a different cursorId than the default one for setUpMongotReturnExplain() so the
            // mock will return the response correctly.
            setUpMongotReturnExplain({
                searchCmd: vectorSearchCmd,
                mongotMock: s0Mongot,
                cursorId: NumberLong(124),
            });
            setUpMongotReturnExplain({
                searchCmd: vectorSearchCmd,
                mongotMock: s1Mongot,
                cursorId: NumberLong(124),
            });

            const result = coll.explain(verbosity).aggregate(pipeline);
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$vectorSearch",
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
        }
        {
            setUpMongotReturnExplainAndCursor({
                mongotMock: s0Mongot,
                coll,
                searchCmd: vectorSearchCmd,
                nextBatch: [
                    {_id: 3, $vectorSearchScore: 100},
                    {_id: 2, $vectorSearchScore: 10},
                    {_id: 4, $vectorSearchScore: 1},
                    {_id: 1, $vectorSearchScore: 0.2},
                ],
            });
            setUpMongotReturnExplainAndCursor({
                mongotMock: s1Mongot,
                coll,
                searchCmd: vectorSearchCmd,
                nextBatch: [
                    {_id: 11, $vectorSearchScore: 100},
                    {_id: 12, $vectorSearchScore: 10},
                    {_id: 13, $vectorSearchScore: 1},
                ],
            });
            const result = coll.explain(verbosity).aggregate(pipeline);
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$vectorSearch",
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
        }
        {
            setUpMongotReturnExplainAndCursorGetMore({
                searchCmd: vectorSearchCmd,
                mongotMock: s0Mongot,
                coll: coll,
                batchList: [
                    [{_id: 3, $vectorSearchScore: 100}, {_id: 2, $vectorSearchScore: 1}],
                    [{_id: 4, $vectorSearchScore: 0.99}, {_id: 1, $vectorSearchScore: 0.98}],
                ]
            });
            setUpMongotReturnExplainAndCursorGetMore({
                searchCmd: vectorSearchCmd,
                mongotMock: s1Mongot,
                coll: coll,
                batchList: [
                    [{_id: 11, $vectorSearchScore: 100}, {_id: 12, $vectorSearchScore: 1}],
                    [{_id: 13, $vectorSearchScore: 0.99}],
                ]
            });
            const result = coll.explain(verbosity).aggregate(pipeline, {cursor: {batchSize: 2}});
            getShardedMongotStagesAndValidateExplainExecutionStats({
                result,
                stageType: "$vectorSearch",
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
        }
    }
    if (verbosity == "queryPlanner") {
        runTestOnPrimaries(runExplainQueryPlannerTest);
        runTestOnSecondaries(runExplainQueryPlannerTest);
    } else {
        runTestOnPrimaries(runExplainExecutionStatsTest);
        runTestOnSecondaries(runExplainExecutionStatsTest);
    }
}

// Test that $vectorSearch works with each explain verbosity.
runExplainTest("queryPlanner");
runTestOnPrimaries((shard0Conn, shard1Conn) =>
                       testLimit(shard0Conn, shard1Conn, "queryPlanner", lowerUserLimit));
runTestOnPrimaries((shard0Conn, shard1Conn) =>
                       testLimit(shard0Conn, shard1Conn, "queryPlanner", higherUserLimit));

if (FeatureFlagUtil.isEnabled(testDB.getMongo(), 'SearchExplainExecutionStats')) {
    runExplainTest("executionStats");
    runTestOnPrimaries((shard0Conn, shard1Conn) =>
                           testLimit(shard0Conn, shard1Conn, "executionStats", lowerUserLimit));
    runTestOnPrimaries((shard0Conn, shard1Conn) =>
                           testLimit(shard0Conn, shard1Conn, "executionStats", higherUserLimit));

    runExplainTest("allPlansExecution");
    runTestOnPrimaries((shard0Conn, shard1Conn) =>
                           testLimit(shard0Conn, shard1Conn, "allPlansExecution", lowerUserLimit));
    runTestOnPrimaries((shard0Conn, shard1Conn) =>
                           testLimit(shard0Conn, shard1Conn, "allPlansExecution", higherUserLimit));
}
stWithMock.stop();
