/**
 * Tests that serverStatus contains the extension.vectorSearch section with all expected metrics.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {checkPlatformCompatibleWithExtensions, withExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod failed to start");

const adminDB = conn.getDB("admin");

(function VectorSearchServerStatusMetricsAppearByDefault() {
    // Get serverStatus and verify the extension.vectorSearch section exists.
    const serverStatus = adminDB.runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);

    // Verify the extension.vectorSearch section exists (section name with dot appears as top-level field).
    assert(
        serverStatus.hasOwnProperty("extension.vectorSearch") && serverStatus["extension.vectorSearch"] != null,
        "serverStatus should have non-null 'extension.vectorSearch' section: " + tojson(serverStatus),
    );

    const vectorSearchSection = serverStatus["extension.vectorSearch"];

    // Verify all four metrics are populated.
    function verifyMetricExistsAndIsPopulated(metricName) {
        assert(
            vectorSearchSection.hasOwnProperty(metricName),
            `"extension.vectorSearch should have '${metricName}' field: ${tojson(vectorSearchSection)}`,
        );
        assert.gte(
            vectorSearchSection[metricName],
            0,
            `'${metricName}' should be non-negative: ${vectorSearchSection[metricName]}`,
        );
    }

    verifyMetricExistsAndIsPopulated("legacyVectorSearchUsed");
    verifyMetricExistsAndIsPopulated("extensionVectorSearchUsed");
    verifyMetricExistsAndIsPopulated("onViewKickbackRetries");
    verifyMetricExistsAndIsPopulated("inSubpipelineKickbackRetries");
})();

(function VectorSearchServerStatusMetricsCanBeRequested() {
    const explicitServerStatus = adminDB.runCommand({serverStatus: 1, "extension.vectorSearch": 1});
    assert.commandWorked(explicitServerStatus);
    assert(
        explicitServerStatus.hasOwnProperty("extension.vectorSearch"),
        "Explicitly requested extension.vectorSearch should be present",
    );
})();

(function extensionVectorSearchServerStatusMetricsCanBeExcluded() {
    const excludedServerStatus = adminDB.runCommand({serverStatus: 1, "extension.vectorSearch": 0});
    assert.commandWorked(excludedServerStatus);
    assert(
        !excludedServerStatus.hasOwnProperty("extension.vectorSearch"),
        "Excluded extension.vectorSearch should not be present: " + tojson(excludedServerStatus),
    );
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
            assert(
                initialServerStatus.hasOwnProperty("extension.vectorSearch"),
                "Initial serverStatus should have extension.vectorSearch section",
            );

            const initialMetrics = initialServerStatus["extension.vectorSearch"];
            const initialExtensionCount = initialMetrics.extensionVectorSearchUsed;
            const initialLegacyCount = initialMetrics.legacyVectorSearchUsed;
            const initialOnViewKickbackCount = initialMetrics.onViewKickbackRetries;
            const initialInSubpipelineKickbackCount = initialMetrics.inSubpipelineKickbackRetries;

            // Run a vector search query to trigger the extension.
            const pipeline = [{$vectorSearch: {}}];
            const result = coll.aggregate(pipeline).toArray();
            assert.eq(result.length, testData.length, "Vector search should return all documents");

            // Get final metrics.
            const finalServerStatus = testAdminDB.runCommand({serverStatus: 1});
            assert.commandWorked(finalServerStatus);
            assert(
                finalServerStatus.hasOwnProperty("extension.vectorSearch"),
                "Final serverStatus should have extension.vectorSearch section",
            );

            const finalMetrics = finalServerStatus["extension.vectorSearch"];

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
            const finalInSubpipelineKickbackCount = finalMetrics.inSubpipelineKickbackRetries;

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
                finalInSubpipelineKickbackCount,
                initialInSubpipelineKickbackCount,
                `inSubpipelineKickbackRetries should remain at ${initialInSubpipelineKickbackCount}, got ${finalInSubpipelineKickbackCount}`,
            );
        },
        ["standalone"],
    );
})();
