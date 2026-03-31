/**
 * Tests that serverStatus contains the extension.vectorSearch section with all expected metrics.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";
import {
    assertOnlyTheseMetricsChanged,
    setupExtensionMetricsTest,
} from "jstests/noPassthrough/libs/extension_server_status_helpers.js";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod failed to start");

const adminDB = conn.getDB("admin");

(function VectorSearchServerStatusMetricsAppearByDefault() {
    // Get serverStatus and verify the extension.vectorSearch section exists.
    const serverStatus = adminDB.runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);

    // Verify the extension.vectorSearch section exists (section name with dot appears as top-level field).
    assert(serverStatus.hasOwnProperty("metrics"));
    assert(serverStatus.metrics.hasOwnProperty("extension"), serverStatus.metrics);
    assert(serverStatus.metrics.extension.hasOwnProperty("vectorSearch"), serverStatus.metrics.extension);
    assert(serverStatus.metrics.extension.vectorSearch != null, serverStatus.metrics.extension);

    const vectorSearchSection = serverStatus.metrics.extension.vectorSearch;
    assert(vectorSearchSection.hasOwnProperty("legacyVectorSearchUsed"), vectorSearchSection);
    assert(vectorSearchSection.hasOwnProperty("extensionVectorSearchUsed"), vectorSearchSection);
    assert(vectorSearchSection.hasOwnProperty("onViewKickbackRetries"), vectorSearchSection);
    assert(vectorSearchSection.hasOwnProperty("inUnionWithKickbackRetries"), vectorSearchSection);
})();

(function VectorSearchServerStatusMetricsCanBeRequested() {
    const explicitServerStatus = adminDB.runCommand({serverStatus: 1, "metrics.extension.vectorSearch": 1});
    assert.commandWorked(explicitServerStatus);

    // Make sure the chain metrics -> extension -> vectorSearch exists.
    assert(explicitServerStatus.hasOwnProperty("metrics"));
    assert(explicitServerStatus.metrics.hasOwnProperty("extension"));
    assert(explicitServerStatus.metrics.extension.hasOwnProperty("vectorSearch"));
})();

MongoRunner.stopMongod(conn);

(function ExtensionVectorSearchUsedGetsIncremented() {
    checkPlatformCompatibleWithExtensions();

    withExtensions(
        {"libvector_search_extension.so": {}},
        (testConn) => {
            const {coll, testData, getMetrics} = setupExtensionMetricsTest(
                testConn,
                "featureFlagVectorSearchExtension",
                "extension.vectorSearch",
            );

            const initialMetrics = getMetrics();
            const result = coll.aggregate([{$vectorSearch: {}}]).toArray();
            assert.eq(result.length, testData.length, "Vector search should return all documents");
            const finalMetrics = getMetrics();

            assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, ["extensionVectorSearchUsed"]);
        },
        ["standalone"],
    );
})();
