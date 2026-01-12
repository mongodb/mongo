/**
 * This test ensures that $vectorSearch works against views and only uses legacy vector search.
 * The desired behavior is that queries pass and have predictable results, and the IFR flag kickback retry
 * gets invoked.
 *
 * TODO SERVER-116994 Update these values when vector search as an extension is supported against views.
 * We will most likely just remove these tests.
 * @tags: [ featureFlagExtensionsAPI ]
 */
import {getParameter, setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {mongotCommandForVectorSearchQuery} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {setUpMongotReturnExplain, setUpMongotReturnExplainAndCursor} from "jstests/with_mongot/mongotmock/lib/utils.js";
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";

const kNumShards = 2;

checkPlatformCompatibleWithExtensions();

/**
 * Sets the featureFlagVectorSearchExtension parameter on all non-config nodes in the cluster.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {boolean} featureFlagValue - The value to set for the feature flag.
 */
function setFeatureFlags(conn, featureFlagValue) {
    setParameterOnAllNonConfigNodes(conn, "featureFlagVectorSearchExtension", featureFlagValue);
}

/**
 * Gets the admin database from a connection object.
 * Works with both mongos and mongod connections.
 *
 * @param {Mongo|Object} connection - The connection object (can be a Mongo instance or connection object).
 * @returns {DB} The admin database instance.
 */
function getAdminDB(connection) {
    let adminDB;
    if (typeof connection.getDB === "function") {
        adminDB = connection.getDB("admin");
    } else {
        assert(typeof connection.getSiblingDB === "function", `Cannot get Admin DB from ${tojson(connection)}`);
        adminDB = connection.getSiblingDB("admin");
    }
    return adminDB;
}

/**
 * Extracts a vector search metric value from a serverStatus response.
 *
 * @param {Object} serverStatus - The serverStatus command response object.
 * @param {string} metricName - The name of the metric to extract from extension.vectorSearch section.
 * @returns {number} The metric value if found, otherwise 0.
 */
function extractVectorSearchMetric(serverStatus, metricName) {
    if (serverStatus.hasOwnProperty("extension.vectorSearch")) {
        const vectorSearchSection = serverStatus["extension.vectorSearch"];
        if (vectorSearchSection.hasOwnProperty(metricName)) {
            return vectorSearchSection[metricName];
        }
    }
    return 0;
}

/**
 * Retrieves a vector search metric from a specific connection by running serverStatus.
 *
 * @param {Mongo|Object} conn - The connection to query (can be mongos or mongod).
 * @param {string} metricName - The name of the metric to retrieve from extension.vectorSearch section.
 * @returns {number} The metric value from the connection's serverStatus.
 */
function getVectorSearchMetricFromConnection(conn, metricName) {
    const serverStatus = getAdminDB(conn).runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);
    return extractVectorSearchMetric(serverStatus, metricName);
}

/**
 * Gets the total count of a vector search metric across the entire cluster.
 * Aggregates the metric from mongos and all shards.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {string} metricName - The name of the metric to retrieve from extension.vectorSearch section.
 * @returns {number} The sum of the metric value across all nodes in the cluster.
 */
function getVectorSearchMetric(conn, metricName) {
    // Get value from mongos or mongod (if standalone).
    let totalCount = getVectorSearchMetricFromConnection(conn, metricName);

    // If connected to mongos, get metrics from all shard primaries.
    // For standalone mongod, we're done (no shards to query).
    const db = conn.getDB ? conn.getDB("admin") : conn.getSiblingDB("admin");
    if (FixtureHelpers.isMongos(db)) {
        const shardPrimaries = FixtureHelpers.getPrimaries(db);
        for (const shardPrimary of shardPrimaries) {
            totalCount += getVectorSearchMetricFromConnection(shardPrimary, metricName);
        }
    }

    return totalCount;
}

function getOnViewKickbackRetryCount(conn) {
    return getVectorSearchMetric(conn, "onViewKickbackRetries");
}

function getLegacyVectorSearchUsedCount(conn) {
    return getVectorSearchMetric(conn, "legacyVectorSearchUsed");
}

function getExtensionVectorSearchUsedCount(conn) {
    return getVectorSearchMetric(conn, "extensionVectorSearchUsed");
}

const kTestDbName = "test";
const kTestCollName = "testColl";
const kTestViewName = "testView";
const kTestIndexName = "vector_index";
const kTestViewPipeline = [{$addFields: {enriched: {$concat: ["$title", " - ", {$toString: "$_id"}]}}}];
const kTestData = [
    {_id: 1, title: "Doc1", embedding: [1.0, 0.0, 0.0]},
    {_id: 2, title: "Doc2", embedding: [0.0, 1.0, 0.0]},
    {_id: 3, title: "Doc3", embedding: [0.0, 0.0, 1.0]},
];
const testVectorSearchIndexSpec = {
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

/**
 * Creates a simple collection, view, and vector search index using the provided connection.
 *
 * @param {Mongo|Object} conn - The connection to use (mongos or mongod).
 * @param {MongotMock} mongotMock - The mongot mock instance for configuring responses.
 * @param {ShardingTest|null} shardingTest - The ShardingTest instance if available, null otherwise.
 * @returns {Collection} The view collection.
 */
function createTestViewAndIndex(conn, mongotMock, shardingTest = null) {
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

    // Check if the view exists using collection metadata.
    const viewExists = testDb.getCollectionInfos({name: kTestViewName, type: "view"}).length > 0;
    if (!viewExists) {
        // Create the view if it doesn't exist.
        assert.commandWorked(testDb.createView(kTestViewName, coll.getName(), kTestViewPipeline));
    }
    const view = testDb[kTestViewName];

    // Create the vector search index.
    const createIndexResponse = {
        ok: 1,
        indexesCreated: [{id: "index-Id", name: kTestIndexName}],
    };
    mongotMock.setMockSearchIndexCommandResponse(createIndexResponse);
    assert.commandWorked(
        testDb.runCommand({
            createSearchIndexes: kTestViewName,
            indexes: [testVectorSearchIndexSpec],
        }),
    );

    return view;
}

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
    for (let i = 0; i < (shardingTest ? kNumShards : 1); i++) {
        setUpMongotReturnExplain({
            mongotMock,
            searchCmd: explainVectorSearchCmd,
            cursorId: startingCursorId++,
        });
    }

    // Set up MongotMock cursor for aggregate query, using a cursorId of 124.
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
    for (let i = 0; i < (shardingTest ? kNumShards : 1); i++) {
        setUpMongotReturnExplainAndCursor({
            mongotMock,
            coll: testDb[collName],
            searchCmd: vectorSearchCmd,
            nextBatch: [],
            cursorId: startingCursorId++,
        });
    }

    // Get the current feature flag value to determine if we expect retries.
    const expectRetry = getParameter(conn, "featureFlagVectorSearchExtension").value;
    // We expect 2 retries in the standalone case - one for the explain and one for the aggregate.
    // In a sharded collections topology, we pre-disable the vector search flag upon detecting
    // a view in ClusterAggregate::retryOnViewError, and therefore don't hit the IFR retry logic.
    const expectedViewKickbackRetryDelta = expectRetry && !FixtureHelpers.isMongos(testDb) ? 2 : 0;

    // Get initial server status to check metrics before running queries.
    const initialRetryCount = getOnViewKickbackRetryCount(conn);
    const initialLegacyCount = getLegacyVectorSearchUsedCount(conn);
    const initialExtensionCount = getExtensionVectorSearchUsedCount(conn);

    assert.commandWorked(view.explain().aggregate([vectorSearchQuery]));
    view.aggregate([vectorSearchQuery]);

    // Check server status after the queries and verify metrics were incremented correctly.
    const finalRetryCount = getOnViewKickbackRetryCount(conn);
    const finalLegacyCount = getLegacyVectorSearchUsedCount(conn);
    const finalExtensionCount = getExtensionVectorSearchUsedCount(conn);

    assert.eq(
        finalRetryCount,
        initialRetryCount + expectedViewKickbackRetryDelta,
        `onViewKickbackRetries should have increased from ${initialRetryCount} to ${initialRetryCount + expectedViewKickbackRetryDelta} when feature flag is ${expectRetry}`,
    );

    // We expect one legacy vector search per query per shard (explain + aggregate).
    const expectedLegacyDelta = shardingTest ? 2 * kNumShards : 2;
    assert.eq(
        finalLegacyCount,
        initialLegacyCount + expectedLegacyDelta,
        `legacyVectorSearchUsed should have increased from ${initialLegacyCount} to ${initialLegacyCount + expectedLegacyDelta} (both explain and regular query use legacy)`,
    );

    assert.eq(
        finalExtensionCount,
        initialExtensionCount,
        `extensionVectorSearchUsed should remain at ${initialExtensionCount} (views always use legacy vector search, never extension)`,
    );
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
    // Run with the feature flag set to false.
    // No IFR retry kickback logic should be triggered. This is because the shards get the feature flag value from
    // the IFR context passed from the router, and the router already has the feature flag disabled.
    runViewVectorSearchTests(conn, mongotMock, false, shardingTest);
}

withExtensionsAndMongot({"libvector_search_extension.so": {}}, runTests, ["sharded"], kNumShards);
