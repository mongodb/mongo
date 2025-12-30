/**
 * Tests that serverStatus contains the extension.vectorSearch section with all expected metrics.
 */
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

// TODO SERVER-115772 Add actual tests verifying the values
