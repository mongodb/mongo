/**
 * Tests that $search and $searchMeta work on views, in $unionWith, and inside
 * $rankFusion/$scoreFusion subpipelines via the IFR flag kickback mechanism.
 *
 * With featureFlagSearchExtension=true: the extension stage is used, detects kickback condition,
 * kicks back to legacy search, and the query succeeds.
 *
 * With featureFlagSearchExtension=false: legacy search is used from the start and the
 * query succeeds without any kickback.
 *
 * @tags: [ featureFlagExtensionsAPI ]
 */
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    createTestView,
    getExtensionSearchUsedCount,
    getLegacySearchUsedCount,
    getNumNodes,
    getSearchInHybridSearchKickbackRetryCount,
    getSearchInUnionWithKickbackRetryCount,
    getSearchOnViewKickbackRetryCount,
    kNumShards,
    kSearchQuery,
    kTestCollName,
    kTestViewName,
    runQueriesAndVerifyMetrics,
    setUpSearchMocks,
} from "jstests/noPassthrough/extensions/ifr_flag_retry_utils.js";

checkPlatformCompatibleWithExtensions();

/**
 * Runs $search or $searchMeta on a view and verifies IFR kickback metrics.
 */
function runSearchViewTest(conn, mongotMock, isSearchMeta, featureFlagValue, shardingTest = null) {
    const {testDb, coll, view} = createTestView(conn, shardingTest);

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        viewName: kTestViewName,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
        shardingTest,
    });

    // In sharded mode, when a shard throws CommandOnShardedViewNotSupportedOnMongod the router's
    // onViewError lambda in ClusterAggregate::runAggregate() adds featureFlagSearchExtension to
    // state.ifrFlagsToDisableOnRetries whenever featureFlagExtensionsInsideHybridSearch is off,
    // so the retry parses the resolved-view pipeline with the extension stage already disabled
    // and bindViewInfo never throws the IFR kickback. In standalone with flag=true the kickback
    // fires once per query; with flag=false legacy runs from the start - no kickback either way.
    const expectedViewKickbackRetryDelta = !shardingTest && featureFlagValue ? 1 : 0;

    const expectedLegacyDelta = getNumNodes(shardingTest);

    const stage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchOnViewKickbackRetryCount,
        retryMetricName: "onViewKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: expectedViewKickbackRetryDelta,
        expectedLegacyDelta,
        queries: [
            () => {
                view.aggregate([stage]).toArray();
            },
        ],
    });
}

/**
 * Runs $search or $searchMeta inside a $unionWith subpipeline on a collection.
 */
function runUnionWithSearchStageTests(
    conn,
    mongotMock,
    isSearchMeta,
    featureFlagValue,
    shardingTest = null,
) {
    const {testDb, coll} = createTestView(conn, shardingTest);

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
        shardingTest,
    });

    // The inUnionWith kickback fires once per query when flag=true (on the node that creates
    // the $unionWith subpipeline). With flag=false, legacy runs from the start.
    const expectedInUnionWithKickbackDelta = featureFlagValue ? 1 : 0;
    const expectedLegacyDelta = getNumNodes(shardingTest);

    const stage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};
    const unionWithStage = {
        $unionWith: {
            coll: coll.getName(),
            pipeline: [stage],
        },
    };

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: expectedInUnionWithKickbackDelta,
        expectedLegacyDelta,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

/**
 * Runs $search or $searchMeta inside a $unionWith subpipeline targeting a view.
 */
function runUnionWithOnViewSearchTests(
    conn,
    mongotMock,
    isSearchMeta,
    featureFlagValue,
    shardingTest = null,
) {
    const {testDb, coll} = createTestView(conn, shardingTest);

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        viewName: kTestViewName,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
        shardingTest,
    });

    const expectedInUnionWithKickbackDelta = featureFlagValue ? 1 : 0;
    const expectedLegacyDelta = getNumNodes(shardingTest);

    const stage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};
    const unionWithStage = {
        $unionWith: {
            coll: kTestViewName,
            pipeline: [stage],
        },
    };

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: expectedInUnionWithKickbackDelta,
        expectedLegacyDelta,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

/**
 * Runs $unionWith targeting a view whose pipeline contains $search or $searchMeta.
 */
function runUnionWithOnViewWithSearchInViewDefinitionTests(
    conn,
    mongotMock,
    isSearchMeta,
    featureFlagValue,
    shardingTest = null,
) {
    const {testDb, coll} = createTestView(conn, shardingTest);

    const searchStage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};
    const searchViewName = kTestViewName + (isSearchMeta ? "_searchMeta" : "_search");
    const searchViewExists =
        testDb.getCollectionInfos({name: searchViewName, type: "view"}).length > 0;
    if (!searchViewExists) {
        assert.commandWorked(testDb.createView(searchViewName, kTestCollName, [searchStage]));
    }

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
        shardingTest,
    });

    const expectedInUnionWithKickbackDelta = featureFlagValue ? 1 : 0;
    const expectedLegacyDelta = getNumNodes(shardingTest);

    const unionWithStage = {
        $unionWith: {
            coll: searchViewName,
            pipeline: [],
        },
    };

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchInUnionWithKickbackRetryCount,
        retryMetricName: "inUnionWithKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: expectedInUnionWithKickbackDelta,
        expectedLegacyDelta,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

/**
 * Runs $search inside $rankFusion and $scoreFusion subpipelines and verifies IFR kickback metrics.
 *
 * The kickback fires when featureFlagSearchExtension is enabled but
 * featureFlagExtensionsInsideHybridSearch is not, causing a retry with legacy $search.
 * When featureFlagSearchExtension is disabled, legacy $search is used from the start.
 */
function runSearchHybridSearchTests(
    conn,
    mongotMock,
    isSearchMeta,
    featureFlagValue,
    shardingTest = null,
) {
    // Only $search is valid in hybrid search.
    if (isSearchMeta) {
        return;
    }
    const {testDb, coll} = createTestView(conn, shardingTest);

    // Each $rankFusion/$scoreFusion pipeline with $search triggers one mongot search per node.
    // Set up mock responses for both queries (rankFusion + scoreFusion).
    for (let i = 0; i < 2; i++) {
        setUpSearchMocks(mongotMock, {
            coll,
            testDb,
            query: kSearchQuery,
            isSearchMeta: false,
            startingCursorId: 300 + i * 10,
            shardingTest,
        });
    }

    // One kickback per hybrid search query (rankFusion + scoreFusion).
    const expectedHybridKickbackRetryDelta = featureFlagValue ? 2 : 0;
    const expectedLegacyDelta = 2 * getNumNodes(shardingTest);

    const searchStage = {$search: kSearchQuery};
    const rankFusionPipeline = [
        {$rankFusion: {input: {pipelines: {searchPipeline: [searchStage]}}}},
    ];
    const scoreFusionPipeline = [
        {
            $scoreFusion: {
                input: {pipelines: {searchPipeline: [searchStage]}, normalization: "none"},
            },
        },
    ];

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchInHybridSearchKickbackRetryCount,
        retryMetricName: "inHybridSearchKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: expectedHybridKickbackRetryDelta,
        expectedLegacyDelta,
        queries: [
            () => {
                assert.commandWorked(
                    testDb.runCommand({
                        aggregate: kTestCollName,
                        pipeline: rankFusionPipeline,
                        cursor: {},
                    }),
                );
            },
            () => {
                assert.commandWorked(
                    testDb.runCommand({
                        aggregate: kTestCollName,
                        pipeline: scoreFusionPipeline,
                        cursor: {},
                    }),
                );
            },
        ],
    });
}

function runTests(conn, mongotMock, shardingTest = null) {
    if (shardingTest) {
        // The IFR retry path interleaves planShardedSearch with per-shard search commands
        // non-deterministically (some pipelines merge on mongos, others on a shard primary),
        // so strict FIFO claiming on the shared mongotmock would cause false mismatches.
        // disableOrderCheck() makes mongotmock claim queued responses by command content.
        mongotMock.disableOrderCheck();
    }
    for (const isSearchMeta of [false, true]) {
        for (const featureFlagValue of [true, false]) {
            setParameterOnAllNonConfigNodes(conn, "featureFlagSearchExtension", featureFlagValue);
            runSearchViewTest(conn, mongotMock, isSearchMeta, featureFlagValue, shardingTest);
            runUnionWithSearchStageTests(
                conn,
                mongotMock,
                isSearchMeta,
                featureFlagValue,
                shardingTest,
            );
            runUnionWithOnViewSearchTests(
                conn,
                mongotMock,
                isSearchMeta,
                featureFlagValue,
                shardingTest,
            );
            runUnionWithOnViewWithSearchInViewDefinitionTests(
                conn,
                mongotMock,
                isSearchMeta,
                featureFlagValue,
                shardingTest,
            );
            runSearchHybridSearchTests(
                conn,
                mongotMock,
                isSearchMeta,
                featureFlagValue,
                shardingTest,
            );
            // TODO SERVER-117259: Add coverage for the $search/$searchMeta-in-$lookup kickback.
        }
    }
    if (shardingTest) {
        // Drop any remaining maybeUnused mocks queued under disableOrderCheck so the cluster's
        // built-in shutdown checks see an empty mongotmock state.
        mongotMock.clearQueuedResponses();
    }
}

withExtensionsAndMongot(
    {"libsearch_extension.so": {}},
    runTests,
    ["standalone", "sharded"],
    {shards: kNumShards},
    {setParameter: {featureFlagExtensionsInsideHybridSearch: false}},
);
