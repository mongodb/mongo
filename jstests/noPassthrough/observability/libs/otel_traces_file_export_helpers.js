import {
    findOtelFilesWithSuffix,
    readJsonlFile,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

/**
 * Generates the name for a tracing directory for the given test name and ensures the directory
 * exists. The name is randomized so the same testName will provide different results on different
 * calls.
 * @param {string} testName - Used as a prefix for the directory name.
 * @returns {string} The absolute path to the newly created directory.
 */
export function createTraceDirectory(testName) {
    if (!Random.isInitialized()) {
        Random.setRandomSeed();
    }
    const traceDir = MongoRunner.toRealPath(`${testName}_otel_traces_${Random.randInt(1000000)}`);
    assert(mkdir(traceDir), `Failed to create trace directory: ${traceDir}`);
    return traceDir;
}

/**
 * Finds trace files in the given directory.
 * @param {string} directory - The directory path to search in.
 * @returns {Array<Object>} An array of file objects (from listFiles()) whose names end with
 *     "-trace.jsonl" (the suffix for the opentelemetry trace files in JSONL format).
 */
function findTraceFiles(directory) {
    return findOtelFilesWithSuffix(directory, "-trace.jsonl");
}

/**
 * Returns all spans from an OTLP trace record as a flat array.
 * @param {Object} record - A raw OTLP JSON record.
 * @returns {Array<Object>} A flat array of span objects across all resource and scope spans.
 */
export function getFlatSpansList(record) {
    let spans = [];
    for (const resourceSpan of record?.resourceSpans ?? []) {
        for (const scopeSpan of resourceSpan.scopeSpans ?? []) {
            for (const span of scopeSpan.spans ?? []) {
                spans.push(span);
            }
        }
    }
    return spans;
}

/**
 * Returns all spans exported to the given trace directory as a flat array.
 * @param {string} directory - The directory path to search in.
 * @returns {Array<Object>} A flat array of every span found across all trace files.
 */
export function getAllSpans(directory) {
    return findTraceFiles(directory).flatMap((file) =>
        readJsonlFile(file.name).flatMap(getFlatSpansList),
    );
}

/**
 * Enables full head-based sampling on a node so root spans are always sampled. Requires the node to
 * have been started with featureFlagOtelTraceSampling.
 *
 * On mongos, commands are entry points and are already registered to be "sampled by default", so
 * raising the default factor here is enough. On a standalone mongod, commands are NOT sampled by
 * default (mongod normally inherits the mongos sampling decision), so callers acting as a mongod
 * entry point must pass the command span names they run in `spanNames` to force those specific
 * spans to be sampled.
 * @param {Mongo} conn - A connection to the node.
 * @param {Array<string>} [spanNames] - Span (command) names to force-sample via per-span overrides.
 */
export function enableFullSampling(conn, spanNames = []) {
    const samplingStrategy = {
        samplingFactor: 1.0,
        tokenBucketRateLimit: {refillRate: 1000000, maxTokens: NumberInt(1000000)},
    };
    const config = {defaultSampling: samplingStrategy};
    if (spanNames.length > 0) {
        config.samples = spanNames.map((name) => ({
            spanSelection: {name},
            samplingStrategy,
        }));
    }
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, openTelemetryTracingSampling: config}),
    );
}
