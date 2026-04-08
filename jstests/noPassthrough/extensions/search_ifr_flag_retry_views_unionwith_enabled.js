/**
 * This test ensures that $search and $searchMeta correctly skip invoking the IFR flag kickback
 * retry when featureFlagExtensionViewsAndUnionWith is enabled. It tests views, $unionWith on
 * collections, and $unionWith on views.
 *
 * Only tests featureFlagSearchExtension=true (extension path). The legacy $search path behaves
 * identically regardless of featureFlagExtensionViewsAndUnionWith and is already covered by
 * search_ifr_flag_retry.js.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionViewsAndUnionWith,
 *   featureFlagSearchExtension,
 * ]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    createTestView,
    getExtensionSearchUsedCount,
    getLegacySearchUsedCount,
    getSearchInUnionWithKickbackRetryCount,
    getSearchOnViewKickbackRetryCount,
    kNumShards,
    kSearchQuery,
    kTestCollName,
    kTestViewName,
    runQueriesAndVerifyMetrics,
} from "jstests/noPassthrough/extensions/ifr_flag_retry_utils.js";

checkPlatformCompatibleWithExtensions();

/**
 * Runs $search or $searchMeta on a view and verifies that no IFR kickback occurs.
 */
function runSearchViewTest(conn, shardingTest, isSearchMeta) {
    const {view} = createTestView(conn, shardingTest);
    const numNodes = shardingTest ? kNumShards : 1;

    const stage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchOnViewKickbackRetryCount,
        retryMetricName: "onViewKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: 0,
        expectedLegacyDelta: 0,
        expectedExtensionDelta: numNodes,
        queries: [
            () => {
                view.aggregate([stage]).toArray();
            },
        ],
    });
}

/**
 * Runs $search or $searchMeta inside a $unionWith subpipeline on a collection.
 * No kickback should occur since featureFlagExtensionViewsAndUnionWith is enabled.
 */
function runUnionWithSearchStageTests(conn, shardingTest, isSearchMeta) {
    const {coll} = createTestView(conn, shardingTest);
    const numNodes = shardingTest ? kNumShards : 1;

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
        expectedRetryDelta: 0,
        expectedLegacyDelta: 0,
        expectedExtensionDelta: numNodes,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

/**
 * Runs $search or $searchMeta inside a $unionWith subpipeline targeting a view.
 * No kickback should occur since featureFlagExtensionViewsAndUnionWith is enabled.
 */
function runUnionWithOnViewSearchTests(conn, shardingTest, isSearchMeta) {
    const {coll} = createTestView(conn, shardingTest);
    const numNodes = shardingTest ? kNumShards : 1;

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
        expectedRetryDelta: 0,
        expectedLegacyDelta: 0,
        expectedExtensionDelta: numNodes,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

/**
 * Runs $unionWith targeting a view whose pipeline contains $search or $searchMeta.
 * No kickback should occur since featureFlagExtensionViewsAndUnionWith is enabled.
 */
function runUnionWithOnViewWithSearchInViewDefinitionTests(conn, shardingTest, isSearchMeta) {
    const {testDb, coll} = createTestView(conn, shardingTest);
    const numNodes = shardingTest ? kNumShards : 1;

    const searchStage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};
    const searchViewName = kTestViewName + (isSearchMeta ? "_searchMeta" : "_search");
    const searchViewExists = testDb.getCollectionInfos({name: searchViewName, type: "view"}).length > 0;
    if (!searchViewExists) {
        assert.commandWorked(testDb.createView(searchViewName, kTestCollName, [searchStage]));
    }

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
        expectedRetryDelta: 0,
        expectedLegacyDelta: 0,
        expectedExtensionDelta: numNodes,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

function runTests(conn, shardingTest = null) {
    for (const isSearchMeta of [false, true]) {
        runSearchViewTest(conn, shardingTest, isSearchMeta);
        runUnionWithSearchStageTests(conn, shardingTest, isSearchMeta);
        runUnionWithOnViewSearchTests(conn, shardingTest, isSearchMeta);
        runUnionWithOnViewWithSearchInViewDefinitionTests(conn, shardingTest, isSearchMeta);
    }
}

withExtensions({"libsearch_extension.so": {}}, runTests, ["standalone", "sharded"], {shards: kNumShards});
