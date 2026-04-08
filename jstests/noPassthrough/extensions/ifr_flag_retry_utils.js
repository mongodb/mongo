/**
 * Shared utility functions for $vectorSearch, $search, and $searchMeta IFR flag retry tests.
 * Provides helpers for metrics, test data, collection/view setup, and mongotmock configuration.
 */
import {getParameter, setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    mongotCommandForVectorSearchQuery,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {setUpMongotReturnExplain, setUpMongotReturnExplainAndCursor} from "jstests/with_mongot/mongotmock/lib/utils.js";

export const kNumShards = 2;
export const kTestDbName = "test";
export const kTestCollName = "testColl";
export const kTestViewName = "testView";
export const kTestIndexName = "vector_index";
export const kTestViewPipeline = [{$addFields: {enriched: {$concat: ["$title", " - ", {$toString: "$_id"}]}}}];
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
    assert.commandWorked(testDb.runCommand({createSearchIndexes: namespace, indexes: [testVectorSearchIndexSpec]}));
}

export function createTestCollectionAndIndex(conn, mongotMock, shardingTest = null) {
    const {testDb, coll} = setupTestCollection(conn, shardingTest);
    createSearchIndex(testDb, mongotMock, kTestCollName);
    return coll;
}

export function createTestViewAndIndex(conn, mongotMock, shardingTest = null) {
    const {testDb, coll} = setupTestCollection(conn, shardingTest);

    const viewExists = testDb.getCollectionInfos({name: kTestViewName, type: "view"}).length > 0;
    if (!viewExists) {
        assert.commandWorked(testDb.createView(kTestViewName, coll.getName(), kTestViewPipeline));
    }
    const view = testDb[kTestViewName];

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
    const numNodes = shardingTest ? kNumShards : 1;

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
    for (let i = 0; i < numNodes; i++) {
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
    for (let i = 0; i < numNodes; i++) {
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
    const numShards = shardingTest ? kNumShards : 1;
    for (let i = 0; i < numShards; i++) {
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

export function setupMockVectorSearchResponsesForHybridSearch(conn, mongotMock, shardingTest = null) {
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
    const numNodes = shardingTest ? kNumShards : 1;

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
    for (let i = 0; i < 2 * numNodes; i++) {
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

    const numNodes = shardingTest ? kNumShards : 1;

    const rankFusionPipeline = [{$rankFusion: {input: {pipelines: {vectorPipeline: [vsQuery]}}}}];

    const scoreFusionPipeline = [
        {$scoreFusion: {input: {pipelines: {vectorPipeline: [vsQuery]}, normalization: "none"}}},
    ];

    const expectedLegacyDelta = 2 * numNodes;

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
                    testDb.runCommand({aggregate: kTestCollName, pipeline: rankFusionPipeline, cursor: {}}),
                );
            },
            () => {
                assert.commandWorked(
                    testDb.runCommand({aggregate: kTestCollName, pipeline: scoreFusionPipeline, cursor: {}}),
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
 * Sets up mongotmock responses needed for a $search or $searchMeta query on a view.
 *
 * With flag=true the flow is:
 *   Extension parses -> bindViewInfo() throws IFR kickback -> retry with
 *   legacy $search/$searchMeta -> one search command to mongot.
 *
 * With flag=false the flow is:
 *   Legacy from start -> one search command to mongot. No kickback.
 *
 * TODO SERVER-123557: Add sharded topology support.
 */
export function setUpSearchMocks(mongotMock, {coll, testDb, viewName = null, query, isSearchMeta, startingCursorId}) {
    const collectionUUID = getUUIDFromListCollections(testDb, coll.getName());
    const collName = coll.getName();
    const dbName = testDb.getName();
    let cursorId = startingCursorId;

    // Legacy $searchMeta does not propagate viewName to mongot.
    const searchViewName = isSearchMeta ? null : viewName;

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

    return cursorId;
}
