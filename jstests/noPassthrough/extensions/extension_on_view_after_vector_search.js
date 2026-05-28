/**
 * Verifies that extension stages are properly validated against views even when they follow a
 * legacy $vectorSearch stage after IFR kickback.
 *
 * @tags: [featureFlagExtensionsAPI, featureFlagExtensionStubParsers]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensionsAndMongot,
} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    createTestViewAndIndex,
    kNumShards,
    kTestDbName,
    kTestViewName,
    vectorSearchQuery,
} from "jstests/noPassthrough/extensions/ifr_flag_retry_utils.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

checkPlatformCompatibleWithExtensions();

function runTests(conn, mongotMock, shardingTest = null) {
    // Enable the vector search extension flag so that the IFR kickback fires on the view.
    setParameterOnAllNonConfigNodes(conn, "featureFlagVectorSearchExtension", true);
    createTestViewAndIndex(conn, mongotMock, shardingTest);

    const testDb = conn.getDB(kTestDbName);

    // [$vectorSearch, $testFoo] on a view should fail. On the first attempt,
    // $_extensionVectorSearch triggers an IFR kickback (not allowed on views). On retry,
    // $vectorSearch runs as legacy but $testFoo is still an extension stage. The server must
    // call bindViewInfo() on all stages — including $testFoo — even though the first stage is a
    // legacy mongot stage. bindViewInfo() rejects non-search/non-vectorSearch extension stages
    // on views with NotImplemented. No mongotmock responses are needed because the query fails
    // during view validation before contacting mongot.
    const result = testDb.runCommand({
        aggregate: kTestViewName,
        pipeline: [{$vectorSearch: vectorSearchQuery}, {$testFoo: {}}],
        cursor: {},
    });
    assert.commandFailedWithCode(
        result,
        ErrorCodes.NotImplemented,
        "Expected extension stage to be rejected on a view after legacy $vectorSearch",
    );
}

withExtensionsAndMongot(
    {"libvector_search_extension.so": {}, "libfoo_mongo_extension.so": {}},
    runTests,
    ["standalone", "sharded"],
    {shards: kNumShards},
    {setParameter: {featureFlagExtensionsInsideHybridSearch: false}},
);
