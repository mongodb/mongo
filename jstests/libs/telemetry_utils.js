/**
 * Utility for checking that the aggregated telemetry metrics are logical (follows sum >= max >=
 * min, and sum = max = min if only one execution).
 */
function verifyMetrics(batch) {
    batch.forEach(element => {
        if (element.metrics.execCount === 1) {
            for (const [metricName, summaryValues] of Object.entries(element.metrics)) {
                // Skip over fields that aren't aggregated metrics with sum/min/max (execCount,
                // lastExecutionMicros).
                if (summaryValues.sum === undefined) {
                    continue;
                }
                const debugInfo = {[metricName]: summaryValues};
                // If there has only been one execution, all metrics should have min, max, and sum
                // equal to each other.
                assert.eq(summaryValues.sum, summaryValues.min, debugInfo);
                assert.eq(summaryValues.sum, summaryValues.max, debugInfo);
                assert.eq(summaryValues.min, summaryValues.max, debugInfo);
            }
        } else {
            for (const [metricName, summaryValues] of Object.entries(element.metrics)) {
                // Skip over fields that aren't aggregated metrics with sum/min/max (execCount,
                // lastExecutionMicros).
                if (summaryValues.sum === undefined) {
                    continue;
                }
                const debugInfo = {[metricName]: summaryValues};
                assert.gte(summaryValues.sum, summaryValues.min, debugInfo);
                assert.gte(summaryValues.sum, summaryValues.max, debugInfo);
                assert.lte(summaryValues.min, summaryValues.max, debugInfo);
            }
        }
    });
}