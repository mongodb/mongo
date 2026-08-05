/**
 * Fixtures that let a tracing test run unchanged against either span exporter: the JSONL file
 * exporter or the OTLP HTTP exporter. Each fixture knows how to configure a node's startup
 * parameters and how to read back the spans the cluster exported.
 *
 * Typical use:
 *
 *     for (const Exporter of kSpanExporters) {
 *         describe(`something (${Exporter.exporterName})`, function () {
 *             before(function () {
 *                 this.exporter = new Exporter(jsTestName());
 *                 this.exporter.start();
 *                 // ... start nodes with this.exporter.startupParams(...) ...
 *             });
 *             after(function () {
 *                 // ... stop nodes ...
 *                 this.exporter.stop();
 *             });
 *         });
 *     }
 */
import {OtelHttpServer} from "jstests/noPassthrough/observability/libs/otel_http_export_helpers.js";
import {
    createTraceDirectory,
    readClusterSpans,
} from "jstests/noPassthrough/observability/libs/otel_traces_file_export_helpers.js";

/**
 * Exports spans as OTLP JSON to a per-node-group trace directory.
 */
export class FileSpanExporter {
    static exporterName = "file exporter";

    constructor(testName) {
        this.testName = testName;
    }

    start() {}

    /**
     * @returns {Object} Startup setParameters enabling the exporter.
     */
    startupParams() {
        return {
            openTelemetryTracingFileFlushCount: 1,
            opentelemetryTraceDirectory: createTraceDirectory(this.testName),
        };
    }

    /**
     * @param {Array<Mongo>} conns - Connections to the nodes whose spans should be read.
     * @returns {Array<Object>} Spans annotated with `resource`, deduplicated.
     */
    readSpans(conns) {
        return readClusterSpans(conns);
    }

    stop() {}
}

/**
 * Exports spans over OTLP/HTTP to a mock collector shared by every node of the cluster.
 */
export class HttpSpanExporter {
    static exporterName = "http exporter";

    constructor(testName) {
        this.server = new OtelHttpServer(testName);
    }

    start() {
        this.server.start();
    }

    /**
     * @returns {Object} Startup setParameters enabling the exporter.
     */
    startupParams() {
        return {opentelemetryHttpEndpoint: this.server.getTracesEndpoint()};
    }

    /**
     * The http server receives the spans of every node, so the connections are ignored.
     * @returns {Array<Object>} Spans annotated with `resource`, deduplicated.
     */
    readSpans(conns) {
        return this.server.readSpans();
    }

    stop() {
        this.server.stop();
    }
}

export const kSpanExporters = [FileSpanExporter, HttpSpanExporter];
