/**
 * Shared utility functions for $vectorSearch, $search, and $searchMeta IFR flag retry tests.
 * Provides helpers for metrics, test data, collection/view setup, and mongotmock configuration.
 */
import {
    getParameter,
    setParameterOnAllNonConfigNodes,
} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    getDefaultProtocolVersionForPlanShardedSearch,
    mockPlanShardedSearchResponseOnConn,
    mongotCommandForQuery,
    mongotCommandForVectorSearchQuery,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    setUpMongotReturnExplain,
    setUpMongotReturnExplainAndCursor,
    setUpMongotReturnExplainAndMultiCursor,
} from "jstests/with_mongot/mongotmock/lib/utils.js";

export const kNumShards = 2;
export const kTestDbName = "test";
export const kTestCollName = "testColl";
export const kTestViewName = "testView";
export const kTestIndexName = "vector_index";
export const kTestViewPipeline = [
    {$addFields: {enriched: {$concat: ["$title", " - ", {$toString: "$_id"}]}}},
];
export const kTestData = [
    {_id: 1, title: "Doc1", embedding: [1.0, 0.0, 0.0]},
    {_id: 2, title: "Doc2", embedding: [0.0, 1.0, 0.0]},
    {_id: 3, title: "Doc3", embedding: [0.0, 0.0, 1.0]},
];

/**
 * Gets a metric from serverStatus.metrics.extension[extensionName][metricName] on a single connection.
 */
function getExtensionMetricFromConnection(conn, extensionName, metricName) {
    const serverStatus = conn.getDB("admin").runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);
    return serverStatus.metrics.extension[extensionName][metricName];
}

/**
 * Gets the total count of a metric across the entire cluster (mongos + all shard primaries).
 */
export function getExtensionMetric(conn, extensionName, metricName) {
    let totalCount = getExtensionMetricFromConnection(conn, extensionName, metricName);

    const adminDb = conn.getDB("admin");
    if (FixtureHelpers.isMongos(adminDb)) {
        const shardPrimaries = FixtureHelpers.getPrimaries(adminDb);
        for (const shardPrimary of shardPrimaries) {
            totalCount += getExtensionMetricFromConnection(shardPrimary, extensionName, metricName);
        }
    }

    return totalCount;
}

/**
 * Sets up the test collection with data and optional sharding.
 */
function setupTestCollection(conn, shardingTest = null) {
    const testDb = conn.getDB(kTestDbName);
    const coll = testDb[kTestCollName];
    coll.drop();
    assert.commandWorked(coll.insertMany(kTestData));

    if (shardingTest) {
        shardingTest.shardColl(
            coll.getName(),
            {_id: 1}, // shard key
            {_id: 2}, // split at
            {_id: 2}, // move chunk containing this value
            kTestDbName,
            true, // waitForDelete
        );
    }

    return {testDb, coll};
}

/**
 * Returns the number of data-bearing nodes (kNumShards in sharded mode, 1 in standalone).
 */
export function getNumNodes(shardingTest) {
    return shardingTest ? kNumShards : 1;
}

/**
 * Creates a test collection and view (no search index).
 * @returns {Object} {testDb, coll, view}
 */
export function createTestView(conn, shardingTest = null) {
    const {testDb, coll} = setupTestCollection(conn, shardingTest);

    const viewExists = testDb.getCollectionInfos({name: kTestViewName, type: "view"}).length > 0;
    if (!viewExists) {
        assert.commandWorked(testDb.createView(kTestViewName, coll.getName(), kTestViewPipeline));
    }
    const view = testDb[kTestViewName];

    return {testDb, coll, view};
}

/**
 * Runs queries and verifies IFR kickback metrics.
 *
 * @param {Object} options
 * @param {Mongo} options.conn - The connection to use.
 * @param {function} options.getRetryCountFn - Function to get the retry count metric.
 * @param {string} options.retryMetricName - Name of retry metric for error messages.
 * @param {function} options.getLegacyCountFn - Function to get the legacy usage count.
 * @param {function} options.getExtensionCountFn - Function to get the extension usage count.
 * @param {string} options.featureFlagName - The IFR feature flag name for logging.
 * @param {number} options.expectedRetryDelta - Expected retry count delta.
 * @param {number} options.expectedLegacyDelta - Expected legacy usage delta.
 * @param {number} [options.expectedExtensionDelta=0] - Expected extension usage delta.
 * @param {Array<function>} options.queries - Functions to run (e.g. explain and/or aggregate).
 */
export function runQueriesAndVerifyMetrics({
    conn,
    getRetryCountFn,
    retryMetricName,
    getLegacyCountFn,
    getExtensionCountFn,
    featureFlagName,
    expectedRetryDelta,
    expectedLegacyDelta,
    expectedExtensionDelta = 0,
    queries,
}) {
    const initialRetryCount = getRetryCountFn(conn);
    const initialLegacyCount = getLegacyCountFn(conn);
    const initialExtensionCount = getExtensionCountFn(conn);

    for (const query of queries) {
        query();
    }

    const finalRetryCount = getRetryCountFn(conn);
    const finalLegacyCount = getLegacyCountFn(conn);
    const finalExtensionCount = getExtensionCountFn(conn);

    const featureFlagValue = getParameter(conn, featureFlagName).value;

    assert.eq(
        finalRetryCount,
        initialRetryCount + expectedRetryDelta,
        `${retryMetricName} should have changed from ${initialRetryCount} to ` +
            `${initialRetryCount + expectedRetryDelta} when feature flag is ${featureFlagValue}`,
    );

    assert.eq(
        finalLegacyCount,
        initialLegacyCount + expectedLegacyDelta,
        `legacyUsed should have changed from ${initialLegacyCount} to ` +
            `${initialLegacyCount + expectedLegacyDelta} when feature flag is ${featureFlagValue}`,
    );

    assert.eq(
        finalExtensionCount,
        initialExtensionCount + expectedExtensionDelta,
        `extensionUsed should have changed from ${initialExtensionCount} to ` +
            `${initialExtensionCount + expectedExtensionDelta} when feature flag is ${featureFlagValue}`,
    );
}

// ============================================================================
// $vectorSearch-specific helpers
// ============================================================================

export const testVectorSearchIndexSpec = {
    name: kTestIndexName,
    type: "vectorSearch",
    definition: {
        fields: [{type: "vector", numDimensions: 3, path: "embedding", similarity: "euclidean"}],
    },
};

export const vectorSearchQuery = {
    queryVector: [0.5, 0.5, 0.5],
    path: "embedding",
    numCandidates: 10,
    limit: 5,
    index: kTestIndexName,
};

export function getOnViewKickbackRetryCount(conn) {
    return getExtensionMetric(conn, "vectorSearch", "onViewKickbackRetries");
}

export function getLegacyVectorSearchUsedCount(conn) {
    return getExtensionMetric(conn, "vectorSearch", "legacyVectorSearchUsed");
}

export function getExtensionVectorSearchUsedCount(conn) {
    return getExtensionMetric(conn, "vectorSearch", "extensionVectorSearchUsed");
}

export function getInUnionWithKickbackRetryCount(conn) {
    return getExtensionMetric(conn, "vectorSearch", "inUnionWithKickbackRetries");
}

function getInHybridSearchKickbackRetryCount(conn) {
    return getExtensionMetric(conn, "vectorSearch", "inHybridSearchKickbackRetries");
}

function createSearchIndex(testDb, mongotMock, namespace) {
    const createIndexResponse = {
        ok: 1,
        indexesCreated: [{id: "index-Id", name: kTestIndexName}],
    };
    mongotMock.setMockSearchIndexCommandResponse(createIndexResponse);
    assert.commandWorked(
        testDb.runCommand({createSearchIndexes: namespace, indexes: [testVectorSearchIndexSpec]}),
    );
}

export function createTestCollectionAndIndex(conn, mongotMock, shardingTest = null) {
    const {testDb, coll} = setupTestCollection(conn, shardingTest);
    createSearchIndex(testDb, mongotMock, kTestCollName);
    return coll;
}

export function createTestViewAndIndex(conn, mongotMock, shardingTest = null) {
    const {testDb, view} = createTestView(conn, shardingTest);
    createSearchIndex(testDb, mongotMock, kTestViewName);
    return view;
}

export function setupMockVectorSearchResponsesForView(conn, mongotMock, shardingTest = null) {
    const queryVector = [0.5, 0.5, 0.5];
    const path = "embedding";
    const numCandidates = 10;
    const limit = 5;
    const index = kTestIndexName;
    const vsQuery = {
        $vectorSearch: {queryVector, path, numCandidates, limit, index},
    };

    const testDb = conn.getDB(kTestDbName);
    const collectionUUID = getUUIDFromListCollections(testDb, kTestCollName);

    const dbName = testDb.getName();
    const collName = kTestCollName;
    const viewName = kTestViewName;

    const explainVectorSearchCmd = mongotCommandForVectorSearchQuery({
        queryVector,
        path,
        numCandidates,
        limit,
        index,
        explain: {verbosity: "queryPlanner"},
        collName,
        dbName,
        collectionUUID,
    });
    let startingCursorId = 123;
    for (let i = 0; i < getNumNodes(shardingTest); i++) {
        setUpMongotReturnExplain({
            mongotMock,
            searchCmd: explainVectorSearchCmd,
            cursorId: startingCursorId++,
            maybeUnused: true,
        });
    }

    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        queryVector,
        path,
        numCandidates,
        limit,
        index,
        collName,
        dbName,
        collectionUUID,
    });
    vectorSearchCmd.viewName = viewName;
    vectorSearchCmd.view = {name: viewName, effectivePipeline: kTestViewPipeline};
    for (let i = 0; i < getNumNodes(shardingTest); i++) {
        setUpMongotReturnExplainAndCursor({
            mongotMock,
            coll: testDb[collName],
            searchCmd: vectorSearchCmd,
            nextBatch: [],
            cursorId: startingCursorId++,
            maybeUnused: true,
        });
    }

    return {vectorSearchStage: vsQuery, testDb};
}

export function setUpMongotMockForVectorSearch(
    mongotMock,
    {
        vectorSearchQuery: vsQuery,
        testDb,
        shardingTest,
        startingCursorId,
        coll,
        verbosity = null,
        numPipelineExecutionsPerNode = 1,
        viewName = null,
        viewPipeline = null,
    },
) {
    const collectionUUID = getUUIDFromListCollections(testDb, coll.getName());

    const baseCmdParams = {
        ...vsQuery,
        collName: coll.getName(),
        dbName: testDb.getName(),
        collectionUUID,
    };

    if (verbosity) {
        baseCmdParams.explain = {verbosity};
    }

    const searchCmd = mongotCommandForVectorSearchQuery(baseCmdParams);

    if (viewName && viewPipeline) {
        searchCmd.viewName = viewName;
        searchCmd.view = {name: viewName, effectivePipeline: viewPipeline};
    }

    let cursorId = startingCursorId;
    for (let i = 0; i < getNumNodes(shardingTest); i++) {
        for (let j = 0; j < numPipelineExecutionsPerNode; j++) {
            setUpMongotReturnExplainAndCursor({
                mongotMock,
                coll,
                searchCmd,
                nextBatch: [],
                cursorId: cursorId++,
            });
        }
    }
    return cursorId;
}

export function setupMockVectorSearchResponsesForHybridSearch(
    conn,
    mongotMock,
    shardingTest = null,
) {
    const queryVector = [0.5, 0.5, 0.5];
    const path = "embedding";
    const numCandidates = 10;
    const limit = 5;
    const index = kTestIndexName;
    const vsQuery = {
        $vectorSearch: {queryVector, path, numCandidates, limit, index},
    };

    const testDb = conn.getDB(kTestDbName);
    const collectionUUID = getUUIDFromListCollections(testDb, kTestCollName);

    const dbName = testDb.getName();
    const collName = kTestCollName;

    const vectorSearchCmd = mongotCommandForVectorSearchQuery({
        queryVector,
        path,
        numCandidates,
        limit,
        index,
        collName,
        dbName,
        collectionUUID,
    });

    let startingCursorId = 200;
    for (let i = 0; i < 2 * getNumNodes(shardingTest); i++) {
        setUpMongotReturnExplainAndCursor({
            mongotMock,
            coll: testDb[collName],
            searchCmd: vectorSearchCmd,
            nextBatch: [],
            cursorId: startingCursorId++,
            maybeUnused: true,
        });
    }

    return {vectorSearchStage: vsQuery, testDb};
}

export function runHybridSearchTests(conn, mongotMock, featureFlagValue, shardingTest = null) {
    setParameterOnAllNonConfigNodes(conn, "featureFlagVectorSearchExtension", featureFlagValue);
    createTestCollectionAndIndex(conn, mongotMock, shardingTest);
    const {vectorSearchStage: vsQuery, testDb} = setupMockVectorSearchResponsesForHybridSearch(
        conn,
        mongotMock,
        shardingTest,
    );

    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    const expectedHybridKickbackRetryDelta = expectRetry ? 2 : 0;

    const rankFusionPipeline = [{$rankFusion: {input: {pipelines: {vectorPipeline: [vsQuery]}}}}];

    const scoreFusionPipeline = [
        {$scoreFusion: {input: {pipelines: {vectorPipeline: [vsQuery]}, normalization: "none"}}},
    ];

    const expectedLegacyDelta = 2 * getNumNodes(shardingTest);

    runQueriesAndVerifyMetrics({
        conn,
        getRetryCountFn: getInHybridSearchKickbackRetryCount,
        retryMetricName: "inHybridSearchKickbackRetries",
        getLegacyCountFn: getLegacyVectorSearchUsedCount,
        getExtensionCountFn: getExtensionVectorSearchUsedCount,
        featureFlagName: "featureFlagVectorSearchExtension",
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

// ============================================================================
// $search/$searchMeta-specific helpers
// ============================================================================

export const kSearchQuery = {query: "foo", path: "title"};

export function getSearchOnViewKickbackRetryCount(conn) {
    return getExtensionMetric(conn, "search", "onViewKickbackRetries");
}

export function getLegacySearchUsedCount(conn) {
    return getExtensionMetric(conn, "search", "legacySearchUsed");
}

export function getExtensionSearchUsedCount(conn) {
    return getExtensionMetric(conn, "search", "extensionSearchUsed");
}

export function getSearchInUnionWithKickbackRetryCount(conn) {
    return getExtensionMetric(conn, "search", "inUnionWithKickbackRetries");
}

export function getSearchInHybridSearchKickbackRetryCount(conn) {
    return getExtensionMetric(conn, "search", "inHybridSearchKickbackRetries");
}

/**
 * Sets up mongotmock responses for a $search or $searchMeta query, possibly on a view.
 *
 * Sharded note: the caller must `mongotMock.disableOrderCheck()` first — planShardedSearch
 * and per-shard search commands interleave non-deterministically. Mocks are queued with
 * `maybeUnused: true` because not every variant fires per subtest. planShardedSearch is
 * queued for both the view and collection namespaces (legacy on a sharded view first targets
 * the view name, then retries with the collection name).
 */
export function setUpSearchMocks(
    mongotMock,
    {coll, testDb, viewName = null, query, isSearchMeta, startingCursorId, shardingTest = null},
) {
    const collectionUUID = getUUIDFromListCollections(testDb, coll.getName());
    const collName = coll.getName();
    const dbName = testDb.getName();
    let cursorId = startingCursorId;

    // Legacy $searchMeta does not propagate viewName to mongot.
    const searchViewName = isSearchMeta ? null : viewName;

    if (shardingTest == null) {
        const searchCmd = mongotCommandForQuery({
            query,
            collName,
            db: dbName,
            collectionUUID,
            viewName: searchViewName,
            optimizationFlags: isSearchMeta ? {omitSearchDocumentResults: true} : null,
        });

        setUpMongotReturnExplainAndCursor({
            mongotMock,
            coll,
            searchCmd,
            nextBatch: [],
            cursorId: cursorId++,
            vars: isSearchMeta ? {SEARCH_META: {}} : null,
        });
    } else {
        // Sharded path: queue planShardedSearch + per-shard search responses on the single
        // shared mongotmock. The caller must have called mongotMock.disableOrderCheck() so that
        // mongotmock claims responses by command content rather than queue position.
        //
        // Queue planShardedSearch for both possible target namespaces (view name and collection
        // name) several times each. Per call, the IFR retry path may issue planShardedSearch
        // 0-2 times depending on whether the query is on a view, the feature-flag value, and
        // where the merger lands; queueing extra copies with maybeUnused: true tolerates retry
        // headroom without coupling mock count to a specific code path. Metric assertions in
        // runQueriesAndVerifyMetrics validate the actual legacy/extension counters.
        const kPlanShardedSearchSlackPerCall = 3;
        const planShardedSearchNamespaces = viewName != null ? [viewName, collName] : [collName];
        const stWithMockShim = {st: shardingTest, getMockConnectedToHost: () => mongotMock};
        for (let i = 0; i < kPlanShardedSearchSlackPerCall; i++) {
            for (const ns of planShardedSearchNamespaces) {
                mockPlanShardedSearchResponseOnConn(
                    ns,
                    query,
                    dbName,
                    undefined,
                    stWithMockShim,
                    shardingTest.s,
                    /*maybeUnused=*/ true,
                    /*explainVerbosity=*/ null,
                    /*hasSearchMetaStage=*/ isSearchMeta,
                );
            }
        }

        const protocolVersion = getDefaultProtocolVersionForPlanShardedSearch();
        const perShardSearchCmd = mongotCommandForQuery({
            query,
            collName,
            db: dbName,
            collectionUUID,
            protocolVersion,
            viewName: searchViewName,
            optimizationFlags: isSearchMeta ? {omitSearchDocumentResults: true} : null,
        });
        // Meta cursor IDs are offset to a disjoint range so they cannot collide with results or
        // planShardedSearch cursor IDs across calls.
        const kMetaCursorOffset = 100000;
        for (let i = 0; i < kNumShards; i++) {
            if (isSearchMeta) {
                setUpMongotReturnExplainAndMultiCursor({
                    mongotMock,
                    coll,
                    searchCmd: perShardSearchCmd,
                    nextBatch: [],
                    metaBatch: [],
                    cursorId: cursorId,
                    metaCursorId: cursorId + kMetaCursorOffset,
                    maybeUnused: true,
                });
                cursorId++;
            } else {
                setUpMongotReturnExplainAndCursor({
                    mongotMock,
                    coll,
                    searchCmd: perShardSearchCmd,
                    nextBatch: [],
                    cursorId: cursorId++,
                    maybeUnused: true,
                });
            }
        }
    }

    return cursorId;
}
