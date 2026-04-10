import {getLatestMetrics} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

export function getIndexStatusMetrics(metricsDir, afterDate) {
    let metrics;
    assert.soon(
        () => {
            metrics = getLatestMetrics(metricsDir);
            return metrics !== null && metrics.time > afterDate.getTime();
        },
        () => `No recent metrics found in ${metricsDir}`,
        30_000,
        200,
        {runHangAnalyzer: false},
    );

    return {
        active: metrics["index_builds.active"]?.value ?? 0,
        started: metrics["index_builds.started"]?.value ?? 0,
        succeeded: metrics["index_builds.succeeded"]?.value ?? 0,
        failed: metrics["index_builds.failed"]?.value ?? 0,
    };
}

export function waitForIndexStatusMetrics(metricsDir, afterDate, predicate, message) {
    let latest;
    assert.soon(
        () => {
            latest = getIndexStatusMetrics(metricsDir, afterDate);
            return predicate(latest);
        },
        () => `${message}: ${tojson(latest)}`,
        30_000,
        200,
        {runHangAnalyzer: false},
    );
    return latest;
}
