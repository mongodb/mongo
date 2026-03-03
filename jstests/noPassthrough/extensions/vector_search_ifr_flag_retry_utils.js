/**
 * Shared utility functions and constants for vector search IFR flag retry tests. This module provides
 * helpers for managing vector search metrics, test data, and view/index setup.
 */
import {getParameter, setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForVectorSearchQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {setUpMongotReturnExplain, setUpMongotReturnExplainAndCursor} from "jstests/with_mongot/mongotmock/lib/utils.js";

// Shared constants for test configuration.
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
export const testVectorSearchIndexSpec = {
    name: kTestIndexName,
    type: "vectorSearch",
    definition: {
        fields: [
            {
                type: "vector",
                numDimensions: 3,
                path: "embedding",
                similarity: "euclidean",
            },
        ],
    },
};

// Vector search query parameters used by tests.
export const vectorSearchQuery = {
    queryVector: [0.5, 0.5, 0.5],
    path: "embedding",
    numCandidates: 10,
    limit: 5,
    index: kTestIndexName,
};

/**
 * Sets the featureFlagVectorSearchExtension parameter on all non-config nodes in the cluster.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {boolean} featureFlagValue - The value to set for the feature flag.
 */
export function setFeatureFlags(conn, featureFlagValue) {
    setParameterOnAllNonConfigNodes(conn, "featureFlagVectorSearchExtension", featureFlagValue);
}

/**
 * Retrieves a vector search metric from a specific connection by running serverStatus.
 *
 * @param {Mongo} conn - The connection to query (can be mongos or mongod).
 * @param {string} metricName - The name of the metric to retrieve from extension.vectorSearch section.
 * @returns {number} The metric value from the connection's serverStatus.
 */
function getVectorSearchMetricFromConnection(conn, metricName) {
    const serverStatus = conn.getDB("admin").runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);
    return serverStatus.metrics.extension.vectorSearch[metricName];
}

/**
 * Gets the total count of a vector search metric across the entire cluster.
 * Aggregates the metric from mongos and all shards.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {string} metricName - The name of the metric to retrieve from extension.vectorSearch section.
 * @returns {number} The sum of the metric value across all nodes in the cluster.
 */
export function getVectorSearchMetric(conn, metricName) {
    // Get value from mongos or mongod (if standalone).
    let totalCount = getVectorSearchMetricFromConnection(conn, metricName);

    // If connected to mongos, also get metrics from all shard primaries.
    const adminDb = conn.getDB("admin");
    if (FixtureHelpers.isMongos(adminDb)) {
        const shardPrimaries = FixtureHelpers.getPrimaries(adminDb);
        for (const shardPrimary of shardPrimaries) {
            totalCount += getVectorSearchMetricFromConnection(shardPrimary, metricName);
        }
    }

    return totalCount;
}

/**
 * Gets the count of onViewKickbackRetries metric across the cluster.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @returns {number} The total count of view kickback retries.
 */
export function getOnViewKickbackRetryCount(conn) {
    return getVectorSearchMetric(conn, "onViewKickbackRetries");
}

/**
 * Gets the count of legacyVectorSearchUsed metric across the cluster.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @returns {number} The total count of legacy vector search usage.
 */
export function getLegacyVectorSearchUsedCount(conn) {
    return getVectorSearchMetric(conn, "legacyVectorSearchUsed");
}

/**
 * Gets the count of extensionVectorSearchUsed metric across the cluster.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @returns {number} The total count of extension vector search usage.
 */
export function getExtensionVectorSearchUsedCount(conn) {
    return getVectorSearchMetric(conn, "extensionVectorSearchUsed");
}

/**
 * Gets the count of inUnionWithKickbackRetries metric across the cluster.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @returns {number} The total count of $unionWith kickback retries.
 */
export function getInUnionWithKickbackRetryCount(conn) {
    return getVectorSearchMetric(conn, "inUnionWithKickbackRetries");
}

/**
 * Gets the count of inHybridSearchKickbackRetries metric across the cluster.
 * This metric tracks retries triggered when $vectorSearch is used inside
 * $rankFusion or $scoreFusion subpipelines.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @returns {number} The total count of hybrid search kickback retries.
 */
export function getInHybridSearchKickbackRetryCount(conn) {
    return getVectorSearchMetric(conn, "inHybridSearchKickbackRetries");
}

/**
 * Sets up the test collection with data and optional sharding.
 * This is a shared helper used by both collection and view index setup functions.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @returns {Object} An object containing { testDb, coll } for use in tests.
 */
function setupTestCollection(conn, shardingTest = null) {
    const testDb = conn.getDB(kTestDbName);
    const coll = testDb[kTestCollName];
    coll.drop();
    assert.commandWorked(coll.insertMany(kTestData));

    // Shard the collection if running in a sharded topology.
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
 * Creates a vector search index on the specified namespace.
 *
 * @param {DB} testDb - The database to create the index in.
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {string} namespace - The collection or view name to create the index on.
 */
function createSearchIndex(testDb, mongotMock, namespace) {
    const createIndexResponse = {
        ok: 1,
        indexesCreated: [{id: "index-Id", name: kTestIndexName}],
    };
    mongotMock.setMockSearchIndexCommandResponse(createIndexResponse);
    assert.commandWorked(
        testDb.runCommand({
            createSearchIndexes: namespace,
            indexes: [testVectorSearchIndexSpec],
        }),
    );
}

/**
 * Creates a test collection with a vector search index on the collection namespace.
 * Use this for tests that query the collection directly (e.g., $unionWith, hybrid search).
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @returns {Collection} The test collection.
 */
export function createTestCollectionAndIndex(conn, mongotMock, shardingTest = null) {
    const {testDb, coll} = setupTestCollection(conn, shardingTest);
    createSearchIndex(testDb, mongotMock, kTestCollName);
    return coll;
}

/**
 * Creates a test collection, view, and vector search index on the view namespace.
 * Use this for tests that query the view (e.g., view vector search tests).
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @returns {Collection} The view collection.
 */
export function createTestViewAndIndex(conn, mongotMock, shardingTest = null) {
    const {testDb, coll} = setupTestCollection(conn, shardingTest);

    // Check if the view exists using collection metadata.
    const viewExists = testDb.getCollectionInfos({name: kTestViewName, type: "view"}).length > 0;
    if (!viewExists) {
        // Create the view if it doesn't exist.
        assert.commandWorked(testDb.createView(kTestViewName, coll.getName(), kTestViewPipeline));
    }
    const view = testDb[kTestViewName];

    createSearchIndex(testDb, mongotMock, kTestViewName);

    return view;
}

/**
 * Sets up MongotMock responses for vector search explain and aggregate queries against a view.
 * This configures the mock to return appropriate responses for both the explain command and
 * the regular aggregate command.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @returns {Object} An object containing { vectorSearchQuery, testDb } for use in tests.
 */
export function setupMockVectorSearchResponsesForView(conn, mongotMock, shardingTest = null) {
    const queryVector = [0.5, 0.5, 0.5];
    const path = "embedding";
    const numCandidates = 10;
    const limit = 5;
    const index = kTestIndexName;
    const vectorSearchQuery = {
        $vectorSearch: {
            queryVector,
            path,
            numCandidates,
            limit,
            index,
        },
    };

    const testDb = conn.getDB(kTestDbName);
    const collectionUUID = getUUIDFromListCollections(testDb, kTestCollName);

    const dbName = testDb.getName();
    const collName = kTestCollName;
    const viewName = kTestViewName;
    const numNodes = shardingTest ? kNumShards : 1;

    // Set up MongotMock cursor for explain query, using a cursorId of 123.
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

    // Set up MongotMock cursor for aggregate query.
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
    vectorSearchCmd.view = {
        name: viewName,
        effectivePipeline: kTestViewPipeline,
    };
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

    return {vectorSearchQuery, testDb};
}

/**
 * Sets up MongotMock for vector search queries (explain or aggregate).
 * This is a more general-purpose helper that can handle various test scenarios including
 * $unionWith tests and view-based queries.
 *
 * @param {MongotMock} mongotMock - The mongot mock instance.
 * @param {Object} options - Configuration options.
 * @param {Object} options.vectorSearchQuery - Parameters for the vector search query.
 * @param {DB} options.testDb - Database object.
 * @param {ShardingTest|null} options.shardingTest - ShardingTest instance or null.
 * @param {number} options.startingCursorId - Starting cursor ID.
 * @param {Collection} options.coll - Collection object.
 * @param {string} [options.verbosity] - Explain verbosity.
 * @param {number} [options.numPipelineExecutionsPerNode=1] - Number of times the pipeline is expected to execute per node.
 * @param {string|null} [options.viewName=null] - View name (if querying a view).
 * @param {Array|null} [options.viewPipeline=null] - View pipeline (if querying a view).
 * @returns {number} The next cursor ID to use.
 */
export function setUpMongotMockForVectorSearch(
    mongotMock,
    {
        vectorSearchQuery,
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
        ...vectorSearchQuery,
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
        searchCmd.view = {
            name: viewName,
            effectivePipeline: viewPipeline,
        };
    }

    let cursorId = startingCursorId;
    const numShards = shardingTest ? kNumShards : 1;
    for (let i = 0; i < numShards; i++) {
        const iterations = numPipelineExecutionsPerNode;
        for (let j = 0; j < iterations; j++) {
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

/**
 * Runs queries and verifies vector search metrics.
 * This helper executes explain and aggregate queries and validates that the expected
 * retry and legacy/extension usage metrics are incremented correctly.
 *
 * @param {Object} options - Configuration options.
 * @param {Mongo} options.conn - The connection to use.
 * @param {DB} options.testDb - The test database.
 * @param {function} options.getRetryCountFn - Function to get retry count metric.
 * @param {string} options.retryMetricName - Name of retry metric for error messages.
 * @param {number} options.expectedRetryDelta - Expected retry count delta.
 * @param {number} options.expectedLegacyDelta - Expected legacy vector search delta.
 * @param {number} [options.expectedExtensionDelta=0] - Expected extension vector search delta.
 * @param {function} options.runExplainQuery - Function to run the explain query.
 * @param {function} options.runAggregateQuery - Function to run the aggregate query.
 * @param {ShardingTest|null} [options.shardingTest=null] - ShardingTest instance or null.
 */
export function runQueriesAndVerifyMetrics({
    conn,
    testDb,
    getRetryCountFn,
    retryMetricName,
    expectedRetryDelta,
    expectedLegacyDelta,
    expectedExtensionDelta = 0,
    runExplainQuery,
    runAggregateQuery,
    shardingTest = null,
}) {
    // Get initial server status to check metrics before running queries.
    const initialRetryCount = getRetryCountFn(conn);
    const initialLegacyCount = getLegacyVectorSearchUsedCount(conn);
    const initialExtensionCount = getExtensionVectorSearchUsedCount(conn);

    runExplainQuery();
    runAggregateQuery();

    // Check server status after the queries and verify metrics were incremented correctly.
    const finalRetryCount = getRetryCountFn(conn);
    const finalLegacyCount = getLegacyVectorSearchUsedCount(conn);
    const finalExtensionCount = getExtensionVectorSearchUsedCount(conn);

    const featureFlagValue = getParameter(conn, "featureFlagVectorSearchExtension").value;

    assert.eq(
        finalRetryCount,
        initialRetryCount + expectedRetryDelta,
        `${retryMetricName} should have changed from ${initialRetryCount} to ` +
            `${initialRetryCount + expectedRetryDelta} when feature flag is ${featureFlagValue}`,
    );

    assert.eq(
        finalLegacyCount,
        initialLegacyCount + expectedLegacyDelta,
        `legacyVectorSearchUsed should have changed from ${initialLegacyCount} to ` +
            `${initialLegacyCount + expectedLegacyDelta} when feature flag is ${featureFlagValue}`,
    );

    assert.eq(
        finalExtensionCount,
        initialExtensionCount + expectedExtensionDelta,
        `extensionVectorSearchUsed should have changed from ${initialExtensionCount} to ` +
            `${initialExtensionCount + expectedExtensionDelta} when feature flag is ${featureFlagValue}`,
    );
}

/**
 * Sets up MongotMock responses for vector search queries used in hybrid search ($rankFusion/$scoreFusion).
 * Unlike setupMockVectorSearchResponses which sets up mocks for views, this sets up mocks for
 * regular $vectorSearch commands on the base collection.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @returns {Object} An object containing { vectorSearchQuery, testDb } for use in tests.
 */
export function setupMockVectorSearchResponsesForHybridSearch(conn, mongotMock, shardingTest = null) {
    const queryVector = [0.5, 0.5, 0.5];
    const path = "embedding";
    const numCandidates = 10;
    const limit = 5;
    const index = kTestIndexName;
    const vectorSearchQuery = {
        $vectorSearch: {
            queryVector,
            path,
            numCandidates,
            limit,
            index,
        },
    };

    const testDb = conn.getDB(kTestDbName);
    const collectionUUID = getUUIDFromListCollections(testDb, kTestCollName);

    const dbName = testDb.getName();
    const collName = kTestCollName;
    const numNodes = shardingTest ? kNumShards : 1;

    // Set up MongotMock cursor for regular (non-explain, non-view) $vectorSearch commands.
    // These are used when $vectorSearch runs inside $rankFusion/$scoreFusion subpipelines.
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

    // We need 2 mocks per node: one for $rankFusion and one for $scoreFusion.
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

    return {vectorSearchQuery, testDb};
}

/**
 * Runs tests for $rankFusion and $scoreFusion with $vectorSearch in subpipelines.
 * These hybrid search stages always trigger the IFR kickback retry to use legacy $vectorSearch,
 * regardless of the featureFlagExtensionViewsAndUnionWith setting.
 *
 * Unlike the view kickback, this triggers on both router and shards.
 *
 * @param {Mongo} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {boolean} featureFlagValue - The value to set for the feature flag.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 */
export function runHybridSearchTests(conn, mongotMock, featureFlagValue, shardingTest = null) {
    setFeatureFlags(conn, featureFlagValue);
    // Reuse the index setup to get a consistent test environment (creates collection and index).
    createTestCollectionAndIndex(conn, mongotMock, shardingTest);
    // Set up mock responses for hybrid search (regular $vectorSearch on base collection, not views).
    const {vectorSearchQuery, testDb} = setupMockVectorSearchResponsesForHybridSearch(conn, mongotMock, shardingTest);

    // Get the current feature flag value to determine if we expect retries.
    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    // Hybrid search stages always trigger the kickback when the extension flag is enabled.
    // Unlike the view kickback, this triggers on both router and shards.
    const expectedHybridKickbackRetryDelta = expectRetry ? 2 : 0;

    const numNodes = shardingTest ? kNumShards : 1;

    // Build $rankFusion and $scoreFusion pipelines with $vectorSearch subpipelines.
    const rankFusionPipeline = [
        {
            $rankFusion: {
                input: {
                    pipelines: {
                        vectorPipeline: [vectorSearchQuery],
                    },
                },
            },
        },
    ];

    const scoreFusionPipeline = [
        {
            $scoreFusion: {
                input: {
                    pipelines: {
                        vectorPipeline: [vectorSearchQuery],
                    },
                    normalization: "none",
                },
            },
        },
    ];

    // We expect one legacy $vectorSearch per hybrid search query per shard (rankFusion + scoreFusion).
    const expectedLegacyDelta = 2 * numNodes;

    runQueriesAndVerifyMetrics({
        conn,
        testDb,
        getRetryCountFn: getInHybridSearchKickbackRetryCount,
        retryMetricName: "inHybridSearchKickbackRetries",
        expectedRetryDelta: expectedHybridKickbackRetryDelta,
        expectedLegacyDelta,
        runExplainQuery: () => {
            // Run $rankFusion aggregate.
            assert.commandWorked(
                testDb.runCommand({aggregate: kTestCollName, pipeline: rankFusionPipeline, cursor: {}}),
            );
        },
        runAggregateQuery: () => {
            // Run $scoreFusion aggregate.
            assert.commandWorked(
                testDb.runCommand({aggregate: kTestCollName, pipeline: scoreFusionPipeline, cursor: {}}),
            );
        },
        shardingTest,
    });
}
