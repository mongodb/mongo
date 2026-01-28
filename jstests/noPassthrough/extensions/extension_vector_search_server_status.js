/**
 * Tests that serverStatus contains the extension.vectorSearch section with all expected metrics.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod failed to start");

const adminDB = conn.getDB("admin");

function getTargetSection(serverStatusOutput) {
    return serverStatusOutput.metrics.extension.vectorSearch;
}

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
            const testAdminDB = testConn.getDB("admin");
            const testDB = testConn.getDB("test");
            const coll = testDB[jsTestName()];
            const testData = [
                {_id: 0, text: "apple"},
                {_id: 1, text: "banana"},
            ];
            assert.commandWorked(coll.insertMany(testData));

            assert.commandWorked(testAdminDB.runCommand({setParameter: 1, featureFlagVectorSearchExtension: true}));

            // Get initial metrics.
            const initialServerStatus = testAdminDB.runCommand({serverStatus: 1});
            assert.commandWorked(initialServerStatus);

            const initialMetrics = getTargetSection(initialServerStatus);
            const initialExtensionCount = initialMetrics.extensionVectorSearchUsed;
            const initialLegacyCount = initialMetrics.legacyVectorSearchUsed;
            const initialOnViewKickbackCount = initialMetrics.onViewKickbackRetries;
            const initialInUnionWithKickbackCount = initialMetrics.inUnionWithKickbackRetries;

            // Run a vector search query to trigger the extension.
            const pipeline = [{$vectorSearch: {}}];
            const result = coll.aggregate(pipeline).toArray();
            assert.eq(result.length, testData.length, "Vector search should return all documents");

            // Get final metrics.
            const finalServerStatus = testAdminDB.runCommand({serverStatus: 1});
            assert.commandWorked(finalServerStatus);

            const finalMetrics = getTargetSection(finalServerStatus);

            // Verify extensionVectorSearchUsed was incremented.
            const finalExtensionCount = finalMetrics.extensionVectorSearchUsed;
            assert.gt(
                finalExtensionCount,
                initialExtensionCount,
                `extensionVectorSearchUsed should have increased from ${initialExtensionCount} to ${finalExtensionCount}:` +
                    tojson(finalMetrics),
            );

            // Verify other metrics remain at 0.
            const finalLegacyCount = finalMetrics.legacyVectorSearchUsed;
            const finalOnViewKickbackCount = finalMetrics.onViewKickbackRetries;
            const finalInUnionWithKickbackCount = finalMetrics.inUnionWithKickbackRetries;

            assert.eq(
                finalLegacyCount,
                initialLegacyCount,
                `legacyVectorSearchUsed should remain at ${initialLegacyCount}, got ${finalLegacyCount}`,
            );
            assert.eq(
                finalOnViewKickbackCount,
                initialOnViewKickbackCount,
                `onViewKickbackRetries should remain at ${initialOnViewKickbackCount}, got ${finalOnViewKickbackCount}`,
            );
            assert.eq(
                finalInUnionWithKickbackCount,
                initialInUnionWithKickbackCount,
                `inUnionWithKickbackRetries should remain at ${initialInUnionWithKickbackCount}, got ${finalInUnionWithKickbackCount}`,
            );
        },
        ["standalone"],
    );
})();
