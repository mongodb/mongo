/**
 * Helpers for testing OpenTelemetry metrics HTTP export.
 */
import {getPython3Binary} from "jstests/libs/python.js";

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
 * Reads captured OTLP HTTP export requests written by otel_metrics_http_server.py.
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

    return content
        .trim()
        .split("\n")
        .map((line) => JSON.parse(line));
}

/**
 * Mock OTLP metrics HTTP server used to validate exporter request headers.
 */
export class OtelMetricsHttpServer {
    constructor(testName) {
        this.testName = testName;
        this.python = getPython3Binary();
        this.serverPy = "jstests/noPassthrough/observability/libs/otel_metrics_http_server.py";
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
            () =>
                rawMongoProgramOutput(".*").search("Mock OTLP Metrics HTTP Server Listening") !==
                -1,
            "Mock OTLP metrics HTTP server failed to start",
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

    waitForRequestWithHeaders(expectedHeaders, timeoutMillis = 30000) {
        assert.soon(
            () => {
                const requests = readCapturedRequests(this.outputFile);
                return requests.some((request) =>
                    requestHasHeaders(request.headers, expectedHeaders),
                );
            },
            "Expected OTLP metrics export request with headers",
            timeoutMillis,
            200,
            {runHangAnalyzer: false},
            {expectedHeaders, outputFile: this.outputFile},
        );
    }
}
