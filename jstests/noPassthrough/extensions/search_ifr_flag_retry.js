/**
 * Tests that $search and $searchMeta work on views via the IFR flag kickback mechanism.
 *
 * With featureFlagSearchExtension=true: the extension stage is used, detects the view,
 * kicks back to legacy search, and the query succeeds.
 *
 * With featureFlagSearchExtension=false: legacy search is used from the start and the
 * query succeeds without any kickback.
 *
 * @tags: [ featureFlagExtensionsAPI ]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    createTestView,
    getExtensionSearchUsedCount,
    getLegacySearchUsedCount,
    getSearchOnViewKickbackRetryCount,
    kSearchQuery,
    kTestViewName,
    runQueriesAndVerifyMetrics,
    setUpSearchMocks,
} from "jstests/noPassthrough/extensions/ifr_flag_retry_utils.js";

checkPlatformCompatibleWithExtensions();

/**
 * Runs $search or $searchMeta on a view and verifies IFR kickback metrics.
 */
function runSearchViewTest(conn, mongotMock, isSearchMeta, featureFlagValue) {
    const {testDb, coll, view} = createTestView(conn);

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        viewName: kTestViewName,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
    });

    // In standalone with flag=true, the IFR kickback fires once per query.
    // With flag=false, legacy runs from the start — no kickback at all.
    const expectedViewKickbackRetryDelta = featureFlagValue ? 1 : 0;

    const stage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getSearchOnViewKickbackRetryCount,
        retryMetricName: "onViewKickbackRetries",
        getLegacyCountFn: getLegacySearchUsedCount,
        getExtensionCountFn: getExtensionSearchUsedCount,
        featureFlagName: "featureFlagSearchExtension",
        expectedRetryDelta: expectedViewKickbackRetryDelta,
        expectedLegacyDelta: 1,
        queries: [
            () => {
                view.aggregate([stage]).toArray();
            },
        ],
    });
}

function runTests(conn, mongotMock) {
    for (const isSearchMeta of [false, true]) {
        for (const featureFlagValue of [true, false]) {
            assert.commandWorked(conn.adminCommand({setParameter: 1, featureFlagSearchExtension: featureFlagValue}));
            runSearchViewTest(conn, mongotMock, isSearchMeta, featureFlagValue);
        }
    }
}

withExtensionsAndMongot(
    {"libsearch_extension.so": {}},
    runTests,
    // TODO SERVER-123557: Add sharded topology testing.
    ["standalone"],
    {},
    {setParameter: {featureFlagExtensionViewsAndUnionWith: false}},
);
