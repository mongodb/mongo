import {
    findOtelFilesWithSuffix,
    readJsonlFile,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

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
