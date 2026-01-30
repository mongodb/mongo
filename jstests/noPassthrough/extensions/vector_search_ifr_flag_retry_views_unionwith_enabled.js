/**
 * This test ensures that $vectorSearch correctly skips invoking the IFR flag kickback retry when
 * featureFlagExtensionViewsAndUnionWith is enabled. It tests views, $unionWith on collections,
 * and $unionWith on views.
 *
 * It also tests that $rankFusion and $scoreFusion with $vectorSearch subpipelines always use
 * legacy vector search via the IFR kickback retry mechanism, regardless of the
 * featureFlagExtensionViewsAndUnionWith setting.
 *
 * @tags: [ featureFlagExtensionsAPI, featureFlagExtensionViewsAndUnionWith ]
 */
import {getParameter} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
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
    runQueriesAndVerifyMetrics,
    runHybridSearchTests,
    setFeatureFlags,
    setUpMongotMockForVectorSearch,
    setupMockVectorSearchResponsesForView,
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
    const {vectorSearchQuery, testDb} = setupMockVectorSearchResponsesForView(conn, mongotMock, shardingTest);

    const numNodes = shardingTest ? kNumShards : 1;

    // Get the current feature flag value to determine if we expect extension or legacy $vectorSearch to be used.
    const expectExtension = getParameter(conn, "featureFlagVectorSearchExtension").value;

    // There should have been no retries since featureFlagExtensionViewsAndUnionWith is enabled.
    // If the extension is used, we expect extension $vectorSearch; otherwise legacy.
    // 2 queries (explain + aggregate) per shard.
    const expectedLegacyDelta = expectExtension ? 0 : 2 * numNodes;
    const expectedExtensionDelta = expectExtension ? 2 * numNodes : 0;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getOnViewKickbackRetryCount,
        retryMetricName: "onViewKickbackRetries",
        expectedRetryDelta: 0,
        expectedLegacyDelta,
        expectedExtensionDelta,
        runExplainQuery: () => {
            assert.commandWorked(view.explain().aggregate([vectorSearchQuery]));
        },
        runAggregateQuery: () => {
            assert.commandWorked(
                testDb.runCommand({aggregate: view.getName(), pipeline: [vectorSearchQuery], cursor: {}}),
            );
        },
        shardingTest,
    });
}

/**
 * Runs the vector search tests within a $unionWith on a collection.
 * When featureFlagExtensionViewsAndUnionWith is enabled, no kickback retry should occur.
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
    const numNodes = shardingTest ? kNumShards : 1;

    // Set up MongotMock cursor for aggregate query.
    setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        shardingTest,
        startingCursorId: 123,
        coll,
        testDb,
    });

    const unionWithStage = {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [{$vectorSearch: vectorSearchQuery}],
        },
    };

    // Get the current feature flag value to determine if we expect extension or legacy $vectorSearch.
    const expectExtension = getParameter(conn, "featureFlagVectorSearchExtension").value;

    // There should have been no retries since featureFlagExtensionViewsAndUnionWith is enabled.
    // 1 aggregate query per shard.
    const expectedLegacyDelta = expectExtension ? 0 : numNodes;
    const expectedExtensionDelta = expectExtension ? numNodes : 0;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        expectedRetryDelta: 0,
        expectedLegacyDelta,
        expectedExtensionDelta,
        runExplainQuery: () => {
            // No explain query in this test - only aggregate.
        },
        runAggregateQuery: () => {
            coll.aggregate([unionWithStage]).toArray();
        },
        shardingTest,
    });
}

/**
 * Runs $vectorSearch within a $unionWith on a view.
 * When featureFlagExtensionViewsAndUnionWith is enabled, no kickback retry should occur.
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
    const numNodes = shardingTest ? kNumShards : 1;

    // Set up MongotMock cursor for aggregate query.
    // When querying $unionWith on a view with featureFlagExtensionViewsAndUnionWith enabled,
    // the mongot command includes viewName and view fields.
    setUpMongotMockForVectorSearch(mongotMock, {
        vectorSearchQuery,
        shardingTest,
        startingCursorId: 123,
        coll,
        testDb,
        viewName: kTestViewName,
        viewPipeline: kTestViewPipeline,
    });

    const unionWithStage = {
        $unionWith: {
            coll: kTestViewName,
            pipeline: [{$vectorSearch: vectorSearchQuery}],
        },
    };

    // Get the current feature flag value to determine if we expect extension or legacy $vectorSearch.
    const expectExtension = getParameter(conn, "featureFlagVectorSearchExtension").value;

    // There should have been no retries since featureFlagExtensionViewsAndUnionWith is enabled.
    // 1 aggregate query per shard.
    const expectedLegacyDelta = expectExtension ? 0 : numNodes;
    const expectedExtensionDelta = expectExtension ? numNodes : 0;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        expectedRetryDelta: 0,
        expectedLegacyDelta,
        expectedExtensionDelta,
        runExplainQuery: () => {
            // No explain query in this test - only aggregate.
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
    // Run with the feature flag set to true. Extension $vectorSearch should be used.
    runViewVectorSearchTests(conn, mongotMock, true, shardingTest);
    runUnionWithVectorSearchTests(conn, mongotMock, true, shardingTest);
    runUnionWithOnViewVectorSearchTests(conn, mongotMock, true, shardingTest);

    // Run with the feature flag set to false. Legacy $vectorSearch should be used.
    runViewVectorSearchTests(conn, mongotMock, false, shardingTest);
    runUnionWithVectorSearchTests(conn, mongotMock, false, shardingTest);
    runUnionWithOnViewVectorSearchTests(conn, mongotMock, false, shardingTest);

    // Run hybrid search tests ($rankFusion/$scoreFusion with $vectorSearch subpipelines).
    // These always trigger the IFR kickback retry regardless of featureFlagExtensionViewsAndUnionWith.
    runHybridSearchTests(conn, mongotMock, true, shardingTest);
    runHybridSearchTests(conn, mongotMock, false, shardingTest);
}

// We don't have to manually enable featureFlagExtensionViewsAndUnionWith since the test will only run if it's enabled.
withExtensionsAndMongot({"libvector_search_extension.so": {}}, runTests, ["standalone", "sharded"], {
    shards: kNumShards,
});
