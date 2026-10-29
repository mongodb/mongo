/**
 * Helpers for testing OpenTelemetry HTTP (OTLP) export. The mock server is signal-neutral: it can
 * receive metrics and/or traces. It records the request path and headers of every request, plus the
 * decoded OTLP payload of trace requests, so it validates exporter routing, custom headers, and the
 * spans that were exported.
 */
import {getPython3Binary} from "jstests/libs/python.js";
import {
    dedupeSpans,
    getSpansWithResource,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

/**
 * Custom headers shared by the HTTP export tests, in the form the mock server is expected to
 * receive them.
 */
export const kCustomExportHeaders = {
    "Authorization": "Bearer test-token",
    "X-Tenant-ID": "acme",
    "ValueWithComma": "combined,value",
};

export const kCustomExportHeadersEncoded = {
    "Authorization": "Bearer test-token",
    "X-Tenant-ID": "acme",
    "ValueWithComma": "combined%2Cvalue",
};

/**
 * Asserts that mongod refuses to start when the given HTTP-export-headers parameter is set to a
 * value with an invalid (non-string) header.
 * @param {string} headersParamName e.g. "openTelemetryMetricsHttpExportHeaders"
 */
export function assertRejectsInvalidHeadersAtStartup(headersParamName) {
    clearRawMongoProgramOutput();
    const dbpath = MongoRunner.dataPath;
    resetDbpath(dbpath);

    const args = MongoRunner.arrOptions("mongod", {
        port: 0,
        dbpath: dbpath,
        setParameter: {[headersParamName]: {"bad-header": 5}},
    });
    const exitCode = runMongoProgram(...args);
    assert.neq(exitCode, 0, "Expected mongod startup to fail for invalid header format");
    const output = rawMongoProgramOutput(".*");
    assert.gte(output.search(/strings or arrays of strings/), 0, output);
}

/**
 * Returns true when all expected headers are present on the request, comparing case-insensitively.
 * @param {Object} requestHeaders
 * @param {Object} expectedHeaders
 * @returns {boolean}
 */
function requestHasHeaders(requestHeaders, expectedHeaders) {
    const normalizedHeaders = {};
    for (const [key, value] of Object.entries(requestHeaders)) {
        normalizedHeaders[key.toLowerCase()] = value;
    }

    for (const [key, value] of Object.entries(expectedHeaders)) {
        if (normalizedHeaders[key.toLowerCase()] !== value) {
            return false;
        }
    }
    return true;
}

/**
 * Reads captured OTLP HTTP export requests written by otel_http_server.py. Some spans may be
 * partially written; such lines are skipped and will parse on a later read once the server has
 * finished writing them.
 * @param {string} outputFile
 * @returns {Array<Object>}
 */
export function readCapturedRequests(outputFile) {
    if (!fileExists(outputFile)) {
        return [];
    }

    const content = cat(outputFile);
    if (!content || content.trim() === "") {
        return [];
    }

    const requests = [];
    for (const line of content.trim().split("\n")) {
        if (line.trim() === "") {
            continue;
        }
        try {
            requests.push(JSON.parse(line));
        } catch (e) {
            // Partially written record; it will be picked up on a later read.
        }
    }
    return requests;
}

/**
 * Mock OTLP HTTP server used to validate exporter request paths and headers for metrics and/or
 * traces.
 */
export class OtelHttpServer {
    constructor(testName) {
        this.testName = testName;
        this.python = getPython3Binary();
        this.serverPy = "jstests/noPassthrough/observability/libs/otel_http_server.py";
        this.pid = undefined;
        this.port = undefined;
        this.outputFile = undefined;
    }

    start() {
        this.port = allocatePort();
        this.outputFile =
            MongoRunner.dataPath + this.testName + "_otel_http_" + this.port + ".jsonl";
        removeFile(this.outputFile);

        // Not a fan of calling a python job a "Mongo program", but this is done elsewhere and
        // all of the utilities around spawning a job and monitoring the PID refer to it as
        // such, and I don't want to do a big infrastructure refactor as part of a single test
        clearRawMongoProgramOutput();
        const args = [
            this.python,
            "-u",
            this.serverPy,
            "--port=" + this.port,
            "--output-file=" + this.outputFile,
        ];
        this.pid = _startMongoProgram({args});

        assert(checkProgram(this.pid));
        assert.soon(
            () => rawMongoProgramOutput(".*").search("Mock OTLP HTTP Server Listening") !== -1,
            "Mock OTLP HTTP server failed to start",
        );
    }

    stop() {
        if (this.pid !== undefined) {
            stopMongoProgramByPid(this.pid);
            this.pid = undefined;
        }
    }

    getMetricsEndpoint() {
        return "http://127.0.0.1:" + this.port + "/v1/metrics";
    }

    getTracesEndpoint() {
        return "http://127.0.0.1:" + this.port + "/v1/traces";
    }

    /**
     * Returns every span exported to this server so far, annotated with the resource attributes of
     * the node that emitted it (see getSpansWithResource()) and deduplicated. A partially written
     * request record is skipped and will be picked up on a later call.
     * @returns {Array<Object>}
     */
    readSpans() {
        return dedupeSpans(readCapturedRequests(this.outputFile).flatMap(getSpansWithResource));
    }

    /**
     * Waits until a captured request carries all of the expected headers. If expectedPath is
     * provided, the request must also have been POSTed to that path (e.g. "/v1/traces").
     * @param {Object} expectedHeaders
     * @param {{expectedPath?: string, timeoutMillis?: number}} [options]
     */
    waitForRequestWithHeaders(expectedHeaders, options = {}) {
        const {expectedPath, timeoutMillis = 30000} = options;
        assert.soon(
            () => {
                const requests = readCapturedRequests(this.outputFile);
                return requests.some(
                    (request) =>
                        (expectedPath === undefined || request.path === expectedPath) &&
                        requestHasHeaders(request.headers, expectedHeaders),
                );
            },
            "Expected OTLP export request with headers",
            timeoutMillis,
            200,
            {runHangAnalyzer: false},
            {expectedHeaders, expectedPath, outputFile: this.outputFile},
        );
    }
}
