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
        active: metrics["mongodb.serverStatus.indexBuilds.active"]?.value ?? 0,
        started: metrics["mongodb.serverStatus.indexBuilds.started"]?.value ?? 0,
        succeeded: metrics["mongodb.serverStatus.indexBuilds.succeeded"]?.value ?? 0,
        failed: metrics["mongodb.serverStatus.indexBuilds.failed"]?.value ?? 0,
        toBeResumed: metrics["mongodb.serverStatus.indexBuilds.to_be_resumed"]?.value ?? 0,
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
