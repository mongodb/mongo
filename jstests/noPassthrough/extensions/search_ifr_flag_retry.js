/**
 * Tests that $search and $searchMeta work on views and in $unionWith via the IFR flag kickback
 * mechanism.
 *
 * With featureFlagSearchExtension=true: the extension stage is used, detects kickback condition,
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
    getSearchInUnionWithKickbackRetryCount,
    getSearchOnViewKickbackRetryCount,
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

/**
 * Runs $search or $searchMeta inside a $unionWith subpipeline on a collection.
 */
function runUnionWithSearchStageTests(conn, mongotMock, isSearchMeta, featureFlagValue) {
    const {testDb, coll} = createTestView(conn);

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
    });

    // The inUnionWith kickback fires once per query when flag=true (on the node that creates
    // the $unionWith subpipeline). With flag=false, legacy runs from the start.
    const expectedInUnionWithKickbackDelta = featureFlagValue ? 1 : 0;

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
        expectedLegacyDelta: 1,
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
function runUnionWithOnViewSearchTests(conn, mongotMock, isSearchMeta, featureFlagValue) {
    const {testDb, coll} = createTestView(conn);

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        viewName: kTestViewName,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
    });

    const expectedInUnionWithKickbackDelta = featureFlagValue ? 1 : 0;

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
        expectedLegacyDelta: 1,
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
function runUnionWithOnViewWithSearchInViewDefinitionTests(conn, mongotMock, isSearchMeta, featureFlagValue) {
    const {testDb, coll} = createTestView(conn);

    const searchStage = isSearchMeta ? {$searchMeta: kSearchQuery} : {$search: kSearchQuery};
    const searchViewName = kTestViewName + (isSearchMeta ? "_searchMeta" : "_search");
    const searchViewExists = testDb.getCollectionInfos({name: searchViewName, type: "view"}).length > 0;
    if (!searchViewExists) {
        assert.commandWorked(testDb.createView(searchViewName, kTestCollName, [searchStage]));
    }

    setUpSearchMocks(mongotMock, {
        coll,
        testDb,
        query: kSearchQuery,
        isSearchMeta,
        startingCursorId: 123,
    });

    const expectedInUnionWithKickbackDelta = featureFlagValue ? 1 : 0;

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
        expectedLegacyDelta: 1,
        queries: [
            () => {
                coll.aggregate([unionWithStage]).toArray();
            },
        ],
    });
}

function runTests(conn, mongotMock) {
    for (const isSearchMeta of [false, true]) {
        for (const featureFlagValue of [true, false]) {
            assert.commandWorked(conn.adminCommand({setParameter: 1, featureFlagSearchExtension: featureFlagValue}));
            runSearchViewTest(conn, mongotMock, isSearchMeta, featureFlagValue);
            runUnionWithSearchStageTests(conn, mongotMock, isSearchMeta, featureFlagValue);
            runUnionWithOnViewSearchTests(conn, mongotMock, isSearchMeta, featureFlagValue);
            runUnionWithOnViewWithSearchInViewDefinitionTests(conn, mongotMock, isSearchMeta, featureFlagValue);
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
