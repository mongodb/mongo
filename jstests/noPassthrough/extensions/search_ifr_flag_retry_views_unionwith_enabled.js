/**
 * This test ensures that $search and $searchMeta correctly skip invoking the IFR flag kickback
 * retry when featureFlagExtensionViewsAndUnionWith is enabled.
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
    getSearchOnViewKickbackRetryCount,
    kNumShards,
    kSearchQuery,
    runQueriesAndVerifyMetrics,
} from "jstests/noPassthrough/extensions/ifr_flag_retry_utils.js";

checkPlatformCompatibleWithExtensions();

/**
 * Runs $search or $searchMeta on a view and verifies that no IFR kickback occurs.
 */
function runSearchViewTest(conn, shardingTest, isSearchMeta) {
    const stageName = isSearchMeta ? "$searchMeta" : "$search";
    const isSharded = shardingTest !== null;

    jsTest.log.info(
        `Running ${stageName} view test (views+unionWith enabled): ` +
            `topology=${isSharded ? "sharded" : "standalone"}`,
    );

    const {view} = createTestView(conn, shardingTest);
    const numNodes = isSharded ? kNumShards : 1;

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

function runTests(conn, shardingTest = null) {
    for (const isSearchMeta of [false, true]) {
        runSearchViewTest(conn, shardingTest, isSearchMeta);
    }
}

withExtensions({"libsearch_extension.so": {}}, runTests, ["standalone", "sharded"], {shards: kNumShards});
