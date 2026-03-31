/**
 * Shared helpers for extension server status metrics tests.
 */

/**
 * Reads a metrics section from a connection's serverStatus.
 *
 * @param {object} conn - A connection (mongod or mongos).
 * @param {string} metricsPath - Dot-separated path under serverStatus.metrics
 *      (e.g., "extension.search").
 */
export function getMetricsSection(conn, metricsPath) {
    const serverStatus = conn.getDB("admin").runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);
    let section = serverStatus.metrics;
    for (const part of metricsPath.split(".")) {
        section = section[part];
    }
    return section;
}

/**
 * Sets up a test environment with a feature flag enabled and a collection with test data.
 * Returns {adminDB, coll, testData, getMetrics} for use in assertions.
 *
 * @param {object} testConn - The connection from withExtensions.
 * @param {string} featureFlag - The feature flag to enable (e.g., "featureFlagSearchExtension").
 * @param {string} metricsPath - Dot-separated path under serverStatus.metrics (e.g., "extension.search").
 */
export function setupExtensionMetricsTest(testConn, featureFlag, metricsPath) {
    const adminDB = testConn.getDB("admin");
    const db = testConn.getDB("test");
    const coll = db[jsTestName()];
    const testData = [
        {_id: 0, text: "apple"},
        {_id: 1, text: "banana"},
    ];
    assert.commandWorked(coll.insertMany(testData));
    assert.commandWorked(adminDB.runCommand({setParameter: 1, [featureFlag]: true}));

    function getMetrics() {
        return getMetricsSection(testConn, metricsPath);
    }

    return {adminDB, coll, testData, getMetrics};
}

/**
 * Asserts that only the specified metric keys increased and all others stayed the same.
 *
 * @param {object} initialMetrics - Metrics snapshot before the operation.
 * @param {object} finalMetrics - Metrics snapshot after the operation.
 * @param {string[]} changedKeys - Keys expected to have increased.
 */
export function assertOnlyTheseMetricsChanged(initialMetrics, finalMetrics, changedKeys) {
    for (const key of Object.keys(initialMetrics)) {
        if (changedKeys.includes(key)) {
            assert.gt(
                finalMetrics[key],
                initialMetrics[key],
                `${key} should have increased from ${initialMetrics[key]}, but is still ${finalMetrics[key]}: ` +
                    tojson(finalMetrics),
            );
        } else {
            assert.eq(
                finalMetrics[key],
                initialMetrics[key],
                `${key} should remain at ${initialMetrics[key]}, got ${finalMetrics[key]}`,
            );
        }
    }
}
