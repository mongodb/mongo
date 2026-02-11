/**
 * Helper utilities for tracking and observing extension metrics in tests.
 *
 * These utilities support multiple metric sources (e.g., command-level metrics, extension-level
 * metrics) and can aggregate metrics across multiple nodes in sharded or replica set environments.
 */

/**
 * Observes changes in extension metrics while running the provided operations.
 *
 * This is a flexible helper that can track any numeric metrics by comparing their values
 * before and after running operations. It supports both successful and failing operations.
 *
 * @param {Array} operationsToRun - Array of operations to execute. Each operation should have:
 *   - operation: A function that performs the operation and returns a result
 *   - expectSuccess: (Optional) Boolean indicating whether the operation is expected to succeed.
 *                     If provided, the helper will assert that the actual outcome matches.
 *                     If not provided (null/undefined), no expectation validation is performed.
 *
 * @param {Function} getMetricsFn - Function that retrieves the current metrics object.
 *   The object should have numeric properties that can be compared before/after.
 *   This function is called twice: once before running operations and once after.
 *
 * @param {Array<string>} metricsToTrack - Array of metric property names to track.
 *   These should correspond to numeric properties in the object returned by getMetricsFn.
 *   Example: ['succeeded', 'failed'] or ['extensionSuccesses', 'extensionFailures']
 *
 * @returns {Object} An object containing:
 *   - nSuccessfulOperations: Number of operations that succeeded (did not throw)
 *   - nFailedOperations: Number of operations that failed (threw an exception)
 *   - metricDeltas: Object mapping each tracked metric name to its delta (after - before)
 *
 * @example
 * // Track command-level metrics from a single node with expectation validation
 * function getExtensionCommandMetrics() {
 *     const serverStatus = assert.commandWorked(db.adminCommand({serverStatus: 1}));
 *     return serverStatus.metrics.commands.aggregate.withExtension;
 * }
 *
 * const result = observeExtensionMetricsChange(
 *   [{operation: () => coll.aggregate([{$metrics: {}}]), expectSuccess: true}],
 *   getExtensionCommandMetrics,
 *   ['succeeded', 'failed']
 * );
 *
 * assert.eq(result.metricDeltas.succeeded, 1);
 * assert.eq(result.metricDeltas.failed, 0);
 *
 * @example
 * // Track extension-level metrics without expectation validation
 * function getTotalMetrics(testDB) {
 *     return {
 *         extensionSuccesses: getTotalExtensionMetrics(testDB, "extensionSuccesses"),
 *         extensionFailures: getTotalExtensionMetrics(testDB, "extensionFailures"),
 *     };
 * }
 *
 * const result = observeExtensionMetricsChange(
 *   [{operation: () => coll.aggregate([{$testFoo: {}}]).toArray()}],  // no expectSuccess
 *   () => getTotalMetrics(db),
 *   ['extensionSuccesses', 'extensionFailures']
 * );
 *
 * assert.gt(result.metricDeltas.extensionSuccesses, 0);
 *
 * @example
 * // Track multiple operations with mixed success/failure expectations
 * const result = observeExtensionMetricsChange(
 *   [
 *     {operation: () => coll.aggregate([{$metrics: {}}]), expectSuccess: true},
 *     {operation: () => coll.aggregate([{$invalidStage: {}}]), expectSuccess: false},
 *   ],
 *   getExtensionCommandMetrics,
 *   ['succeeded', 'failed']
 * );
 *
 * assert.eq(result.nSuccessfulOperations, 1);
 * assert.eq(result.nFailedOperations, 1);
 */
export function observeExtensionMetricsChange(operationsToRun, getMetricsFn, metricsToTrack) {
    const metricsBefore = getMetricsFn();
    let nSuccesses = 0,
        nFailures = 0;

    for (let i = 0; i < operationsToRun.length; i++) {
        const {operation, expectSuccess} = operationsToRun[i];
        const expectSuccessExplicitlySet = expectSuccess !== null && expectSuccess !== undefined;
        try {
            operation();
            nSuccesses++;

            // If expectSuccess is explicitly set to false, the operation should have failed.
            if (expectSuccessExplicitlySet && expectSuccess === false) {
                throw new Error(
                    `Operation ${i} was expected to fail but succeeded. ` +
                        `Set expectSuccess to true or omit it if success is acceptable.`,
                );
            }
        } catch (e) {
            nFailures++;

            // If expectSuccess is explicitly set to true, the operation should have succeeded.
            if (expectSuccessExplicitlySet && expectSuccess === true) {
                throw new Error(`Operation ${i} was expected to succeed but failed with error: ${e.message}`);
            }
        }
    }

    const metricsAfter = getMetricsFn();

    const metricDeltas = {};
    for (const metricName of metricsToTrack) {
        metricDeltas[metricName] = metricsAfter[metricName] - metricsBefore[metricName];
    }

    return {
        nSuccessfulOperations: nSuccesses,
        nFailedOperations: nFailures,
        metricDeltas: metricDeltas,
    };
}
