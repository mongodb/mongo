/**
 * This test ensures that $vectorSearch works against views, within $unionWith on collections,
 * within $unionWith on views, and inside $rankFusion/$scoreFusion subpipelines, and that the IFR flag kickback retry is invoked to fall back
 * to legacy vector search when featureFlagExtensionViewsAndUnionWith is disabled.
 *
 * @tags: [ featureFlagExtensionsAPI ]
 */
import {getParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    createTestCollectionAndIndex,
    createTestViewAndIndex,
    getInUnionWithKickbackRetryCount,
    getOnViewKickbackRetryCount,
    kNumShards,
    kTestCollName,
    kTestDbName,
    kTestViewName,
    kTestViewPipeline,
    runHybridSearchTests,
    runQueriesAndVerifyMetrics,
    setFeatureFlags,
    setUpMongotMockForVectorSearch,
    vectorSearchQuery,
} from "jstests/noPassthrough/extensions/vector_search_ifr_flag_retry_utils.js";

checkPlatformCompatibleWithExtensions();

/**
 * Runs the vector search tests against views.
 * This function sets feature flags, then runs the actual tests.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {boolean} featureFlagValue - The value to set for the feature flag.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 */
function runViewVectorSearchTests(conn, mongotMock, featureFlagValue, shardingTest = null) {
    setFeatureFlags(conn, featureFlagValue);
    const view = createTestViewAndIndex(conn, mongotMock, shardingTest);

    const testDb = conn.getDB(kTestDbName);
    const coll = testDb[kTestCollName];

    // Set up MongotMock cursor for explain query, using a cursorId of 123.
    let cursorId = setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        verbosity: "executionStats",
        shardingTest,
        startingCursorId: 123,
        coll,
        testDb,
        viewName: kTestViewName,
        viewPipeline: kTestViewPipeline,
    });

    // Set up MongotMock cursor for aggregate query.
    setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        shardingTest,
        startingCursorId: cursorId,
        coll,
        testDb,
        viewName: kTestViewName,
        viewPipeline: kTestViewPipeline,
    });

    // Get the current feature flag value to determine if we expect retries.
    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    // We expect 2 retries in the standalone case - one for the explain and one for the aggregate.
    // In a sharded collections topology, we pre-disable the vector search flag upon detecting
    // a view in ClusterAggregate::retryOnViewError, and therefore don't hit the IFR retry logic.
    const expectedViewKickbackRetryDelta = expectRetry && !FixtureHelpers.isMongos(testDb) ? 2 : 0;

    // We expect one legacy vector search per query per shard (explain + aggregate).
    const expectedLegacyDelta = shardingTest ? 2 * kNumShards : 2;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getOnViewKickbackRetryCount,
        retryMetricName: "onViewKickbackRetries",
        expectedRetryDelta: expectedViewKickbackRetryDelta,
        expectedLegacyDelta,
        runExplainQuery: () => {
            assert.commandWorked(view.explain("executionStats").aggregate([{$vectorSearch: vectorSearchQuery}]));
        },
        runAggregateQuery: () => {
            view.aggregate([{$vectorSearch: vectorSearchQuery}]).toArray();
        },
        shardingTest,
    });
}

/**
 * Runs the vector search tests within a $unionWith.
 * This function sets feature flags, then runs the actual tests.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {boolean} featureFlagValue - The value to set for the feature flag.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @param {Object|null} unionWithStage - The $unionWith stage to use. If null, a default stage with $vectorSearch is used.
 * @param {boolean} shouldExplain - Whether to run the explain query.
 * @param {string|null} viewName - The view name that the $unionWith stage should run against.
 *                                  If null, $unionWith will run on the default test collection.
 * @param {Array|null} viewPipeline - The view pipeline if running a view.
 */
function runUnionWithVectorSearchTests({
    conn,
    mongotMock,
    featureFlagValue,
    shardingTest = null,
    unionWithStage = null,
    shouldExplain = true,
    viewName = null,
    viewPipeline = null,
}) {
    setFeatureFlags(conn, featureFlagValue);
    // Create collection with search index on the collection namespace (not the view).
    createTestCollectionAndIndex(conn, mongotMock, shardingTest);

    const testDb = conn.getDB(kTestDbName);
    const coll = testDb[kTestCollName];

    unionWithStage = unionWithStage || {
        $unionWith: {
            coll: viewName ? viewName : coll.getName(),
            pipeline: [{$vectorSearch: vectorSearchQuery}],
        },
    };

    // Standalone $unionWith explain runs the subpipeline twice: once during the initial
    // execution to collect main pipeline execution stats, and once in UnionWith::serialize()
    // to collect execution stats for the subpipeline. Sharded only runs it once because the
    // $unionWith is a merging stage and the merging pipeline is not run during an execution
    // stats-level explain.
    // In a real aggregation, the pipeline is run once per data-bearing node.
    const numExplainPipelineExecutionsPerNode = shardingTest ? 1 : 2;
    const numAggregationPipelineExecutionsPerNode = 1;
    const numNodes = shardingTest ? kNumShards : 1;

    // Get the current feature flag value to determine if we expect retries.
    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    // If we are expecting retries, we should always have 1 per shard for the aggregate,
    // and 1 for the explain, if we are running it.
    // In sharded mode, the kickback retry happens on each shard.
    let expectedUnionWithKickbackRetryDelta = expectRetry ? Number(shouldExplain) + 1 : 0;

    let cursorId = 123;
    if (shouldExplain) {
        // Set up MongotMock cursor for the explain query.
        cursorId = setUpMongotMockForVectorSearch(mongotMock, {
            vectorSearchQuery,
            verbosity: "executionStats",
            shardingTest,
            startingCursorId: cursorId,
            coll,
            testDb,
            numPipelineExecutionsPerNode: numExplainPipelineExecutionsPerNode,
            viewName,
            viewPipeline,
        });
    }

    // Set up MongotMock cursor for the aggregate query.
    setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        shardingTest,
        startingCursorId: cursorId,
        coll,
        testDb,
        numPipelineExecutionsPerNode: numAggregationPipelineExecutionsPerNode,
        viewName,
        viewPipeline,
    });

    const expectedLegacyDelta =
        Number(shouldExplain) * numExplainPipelineExecutionsPerNode * numNodes +
        numAggregationPipelineExecutionsPerNode * numNodes;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        expectedRetryDelta: expectedUnionWithKickbackRetryDelta,
        expectedLegacyDelta,
        runExplainQuery: () => {
            if (shouldExplain) {
                const explain = coll.explain("executionStats").aggregate([unionWithStage]);
                assert.commandWorked(explain);
            }
        },
        runAggregateQuery: () => {
            coll.aggregate([unionWithStage]).toArray();
        },
        shardingTest,
    });
}

function runUnionWithOnViewVectorSearchTests(conn, mongotMock, featureFlagValue, shardingTest = null) {
    // The test driver will set up the view for us. We just need to provide parameters to indicate that
    // we should use it. Skipping explain because $unionWith + a view + explain + legacy $vectorSearch
    // fails (SERVER-117879).
    runUnionWithVectorSearchTests({
        conn,
        mongotMock,
        featureFlagValue,
        shardingTest,
        viewName: kTestViewName,
        viewPipeline: kTestViewPipeline,
        shouldExplain: false,
    });
}

function runUnionWithOnViewWithVectorSearchInViewDefinitionTests(
    conn,
    mongotMock,
    featureFlagValue,
    shardingTest = null,
) {
    const testDb = conn.getDB(kTestDbName);

    // Create a view that runs the $vectorSearch query. The results will look
    // identical to a $unionWith + $vectorSearch on the base collection, but the
    // code path is different because the kickback must happen after view
    // resolution instead of during parsing.
    const vectorSearchViewPipeline = [{$vectorSearch: vectorSearchQuery}];
    const vectorSearchViewName = kTestViewName + "_vectorSearch";
    assert.commandWorked(testDb.createView(vectorSearchViewName, kTestCollName, vectorSearchViewPipeline));

    const unionWithStage = {
        $unionWith: {
            coll: vectorSearchViewName,
            pipeline: [],
        },
    };
    // Skipping explain because $unionWith + a view + explain + legacy $vectorSearch
    // fails (SERVER-117879).
    runUnionWithVectorSearchTests({
        conn,
        mongotMock,
        featureFlagValue,
        shardingTest,
        unionWithStage,
        shouldExplain: false,
    });
}

/**
 * Test function that runs for each topology (standalone and sharded).
 * This is called by withExtensions for each topology.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 */
function runTests(conn, mongotMock, shardingTest = null) {
    // Run with the feature flag set to true. The IFR retry kickback logic should trigger
    // and legacy vector search should be used.
    runViewVectorSearchTests(conn, mongotMock, true, shardingTest);
    runUnionWithVectorSearchTests({
        conn,
        mongotMock,
        featureFlagValue: true,
        shardingTest,
    });
    runUnionWithOnViewVectorSearchTests(conn, mongotMock, true, shardingTest);
    runUnionWithOnViewWithVectorSearchInViewDefinitionTests(conn, mongotMock, true, shardingTest);

    // Run with the feature flag set to false.
    // No IFR retry kickback logic should be triggered. This is because the shards get the feature flag value from
    // the IFR context passed from the router, and the router already has the feature flag disabled.
    runViewVectorSearchTests(conn, mongotMock, false, shardingTest);
    runUnionWithVectorSearchTests({
        conn,
        mongotMock,
        featureFlagValue: false,
        shardingTest,
    });
    runUnionWithOnViewVectorSearchTests(conn, mongotMock, false, shardingTest);
    runUnionWithOnViewWithVectorSearchInViewDefinitionTests(conn, mongotMock, false, shardingTest);

    // Run hybrid search tests ($rankFusion/$scoreFusion with $vectorSearch subpipelines).
    // These always trigger the IFR kickback retry regardless of featureFlagExtensionViewsAndUnionWith.
    runHybridSearchTests(conn, mongotMock, true, shardingTest);
    runHybridSearchTests(conn, mongotMock, false, shardingTest);
}

withExtensionsAndMongot(
    {"libvector_search_extension.so": {}},
    runTests,
    ["standalone", "sharded"],
    {
        shards: kNumShards,
    },
    {
        setParameter: {featureFlagExtensionViewsAndUnionWith: false},
    },
);
