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
 */
function runUnionWithVectorSearchTests(conn, mongotMock, featureFlagValue, shardingTest = null) {
    setFeatureFlags(conn, featureFlagValue);
    // Create collection with search index on the collection namespace (not the view).
    createTestCollectionAndIndex(conn, mongotMock, shardingTest);

    const testDb = conn.getDB(kTestDbName);
    const coll = testDb[kTestCollName];

    // Standalone $unionWith explain runs the subpipeline twice: once during the initial
    // execution to collect main pipeline execution stats, and once in UnionWith::serialize()
    // to collect execution stats for the subpipeline. Sharded only runs it once because the
    // $unionWith is a merging stage and the merging pipeline is not run during an execution
    // stats-level explain.
    // In a real aggregation, the pipeline is run once per data-bearing node.
    const numExplainPipelineExecutionsPerNode = shardingTest ? 1 : 2;
    const numAggregationPipelineExecutionsPerNode = 1;
    const numNodes = shardingTest ? kNumShards : 1;

    // Set up MongotMock cursor for explain query, using a cursorId of 123.
    let cursorId = setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        verbosity: "executionStats",
        shardingTest,
        startingCursorId: 123,
        coll,
        testDb,
        numPipelineExecutionsPerNode: numExplainPipelineExecutionsPerNode,
    });

    const unionWithStage = {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [{$vectorSearch: vectorSearchQuery}],
        },
    };

    // Get the current feature flag value to determine if we expect retries.
    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    // We expect 1 retry per query per shard - one for the explain and one for the aggregate.
    // In sharded mode, the kickback retry happens on each shard.
    const expectedUnionWithKickbackRetryDelta = expectRetry ? 2 * numNodes : 0;

    // Set up MongotMock cursor for aggregate query.
    setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        shardingTest,
        startingCursorId: cursorId,
        coll,
        testDb,
        numPipelineExecutionsPerNode: numAggregationPipelineExecutionsPerNode,
    });

    const expectedLegacyDelta =
        numExplainPipelineExecutionsPerNode * numNodes + numAggregationPipelineExecutionsPerNode * numNodes;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        expectedRetryDelta: expectedUnionWithKickbackRetryDelta,
        expectedLegacyDelta,
        runExplainQuery: () => {
            const explain = coll.explain("executionStats").aggregate([unionWithStage]);
            assert.commandWorked(explain);
        },
        runAggregateQuery: () => {
            coll.aggregate([unionWithStage]).toArray();
        },
        shardingTest,
    });
}

/**
 * Runs $vectorSearch within a $unionWith on a view.
 * This function sets feature flags, then runs the actual tests.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {boolean} featureFlagValue - The value to set for the feature flag.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 */
function runUnionWithOnViewVectorSearchTests(conn, mongotMock, featureFlagValue, shardingTest = null) {
    setFeatureFlags(conn, featureFlagValue);
    createTestViewAndIndex(conn, mongotMock, shardingTest);

    const testDb = conn.getDB(kTestDbName);
    const coll = testDb[kTestCollName];
    const viewName = kTestViewName;

    const numNodes = shardingTest ? kNumShards : 1;

    const unionWithStage = {
        $unionWith: {
            coll: viewName,
            pipeline: [{$vectorSearch: vectorSearchQuery}],
        },
    };

    // Get the current feature flag value to determine if we expect retries.
    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    // We expect 1 retry per shard for the aggregate (no explain query is run in this test).
    const expectedUnionWithOnViewKickbackRetryDelta = expectRetry ? numNodes : 0;

    // Set up MongotMock cursor for aggregate query.
    setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        shardingTest,
        startingCursorId: 123,
        coll,
        testDb,
        viewName,
        viewPipeline: kTestViewPipeline,
    });

    // We expect one legacy vector search per query per shard.
    const expectedLegacyDelta = shardingTest ? kNumShards : 1;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        expectedRetryDelta: expectedUnionWithOnViewKickbackRetryDelta,
        expectedLegacyDelta,
        runExplainQuery: () => {
            // Skipping this since $unionWith + legacy $vectorSearch + explain
            // executionStats on a view fails (SERVER-117879).
        },
        runAggregateQuery: () => {
            coll.aggregate([unionWithStage]).toArray();
        },
        shardingTest,
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
    runUnionWithVectorSearchTests(conn, mongotMock, true, shardingTest);
    runUnionWithOnViewVectorSearchTests(conn, mongotMock, true, shardingTest);

    // Run with the feature flag set to false.
    // No IFR retry kickback logic should be triggered. This is because the shards get the feature flag value from
    // the IFR context passed from the router, and the router already has the feature flag disabled.
    runViewVectorSearchTests(conn, mongotMock, false, shardingTest);
    runUnionWithVectorSearchTests(conn, mongotMock, false, shardingTest);
    runUnionWithOnViewVectorSearchTests(conn, mongotMock, false, shardingTest);

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
