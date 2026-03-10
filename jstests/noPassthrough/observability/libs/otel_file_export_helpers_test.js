/**
 * Unit tests for getLatestMetrics in otel_file_export_helpers.js.
 *
 * Tests file/record selection logic, accumulation of sum and gauge data points,
 * histogram passthrough, and the time property calculation.
 */

import {describe, it} from "jstests/libs/mochalite.js";
import {
    extractPrometheusMetricIntValue,
    extractPrometheusMetricTime,
    getLatestMetrics,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

function makeRecord(metrics) {
    return {
        resourceMetrics: [
            {
                resource: {attributes: []},
                scopeMetrics: [
                    {
                        metrics: metrics,
                        scope: {name: "mongodb"},
                    },
                ],
            },
        ],
    };
}

function makeSumMetric(name, dataPoints) {
    return {
        name,
        sum: {aggregationTemporality: 2, dataPoints, isMonotonic: true},
    };
}

function makeGaugeMetric(name, dataPoints) {
    return {name, gauge: {dataPoints}};
}

function makeHistogramMetric(name, dataPoints) {
    return {name, histogram: {aggregationTemporality: 2, dataPoints}};
}

function makeDataPoint(value, timeUnixNano, isInt = true) {
    const dp = {
        startTimeUnixNano: "1000000000000000",
        timeUnixNano: String(timeUnixNano),
    };
    if (isInt) {
        dp.asInt = String(value);
    } else {
        dp.asDouble = value;
    }
    return dp;
}

function makeHistogramDataPoint(timeUnixNano, opts) {
    return {
        startTimeUnixNano: "1000000000000000",
        timeUnixNano: String(timeUnixNano),
        count: String(opts.count),
        sum: opts.sum,
        min: opts.min,
        max: opts.max,
        bucketCounts: opts.bucketCounts.map(String),
        explicitBounds: opts.explicitBounds,
    };
}

function writeMetricsFile(dir, filename, records) {
    const content = records.map((r) => JSON.stringify(r)).join("\n") + "\n";
    writeFile(dir + "/" + filename, content);
}

function createTestDir(suffix) {
    const dir = MongoRunner.toRealPath("otel_test_" + suffix + "_" + ObjectId().str);
    assert(mkdir(dir));
    return dir;
}

const kTime1Ns = "1000000000000000000";
const kTime1Ms = Number(BigInt(kTime1Ns) / 1_000_000n);
const kTime2Ns = "2000000000000000000";
const kTime2Ms = Number(BigInt(kTime2Ns) / 1_000_000n);
const kTime3Ns = "3000000000000000000";
const kTime3Ms = Number(BigInt(kTime3Ns) / 1_000_000n);

describe("getLatestMetrics", function () {
    describe("file and record selection", function () {
        it("returns null when no metrics files exist in the directory", function () {
            const dir = createTestDir("no_files");
            const result = getLatestMetrics(dir);
            assert.eq(result, null);
        });

        it("ignores files that do not end with -metrics.jsonl", function () {
            const dir = createTestDir("non_metrics");
            writeFile(dir + "/something.json", '{"key": "value"}\n');
            writeFile(dir + "/other.txt", "hello\n");
            const result = getLatestMetrics(dir);
            assert.eq(result, null);
        });

        it("returns null when all metrics files are empty", function () {
            const dir = createTestDir("all_empty");
            writeFile(dir + "/a-metrics.jsonl", "");
            writeFile(dir + "/b-metrics.jsonl", "");
            const result = getLatestMetrics(dir);
            assert.eq(result, null, "Should return null when all files are empty");
        });

        it("selects the record with the latest time across multiple files", function () {
            const dir = createTestDir("latest_across_files");

            writeMetricsFile(dir, "file1-metrics.jsonl", [
                makeRecord([makeSumMetric("m.early", [makeDataPoint(10, kTime1Ns)])]),
            ]);
            writeMetricsFile(dir, "file2-metrics.jsonl", [
                makeRecord([makeSumMetric("m.latest", [makeDataPoint(30, kTime3Ns)])]),
            ]);
            writeMetricsFile(dir, "file3-metrics.jsonl", [
                makeRecord([makeSumMetric("m.middle", [makeDataPoint(20, kTime2Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.neq(result, null);
            assert(result.hasOwnProperty("m.latest"), "Should contain metric from latest file");
            assert.eq(result["m.latest"].value, 30);
            assert(!result.hasOwnProperty("m.early"), "Should not contain metrics from older files");
            assert(!result.hasOwnProperty("m.middle"), "Should not contain metrics from older files");
        });

        it("uses only the last record from each file", function () {
            const dir = createTestDir("last_record");

            writeMetricsFile(dir, "file1-metrics.jsonl", [
                makeRecord([makeSumMetric("m.older", [makeDataPoint(1, kTime1Ns)])]),
                makeRecord([makeSumMetric("m.newer", [makeDataPoint(2, kTime2Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.neq(result, null);
            assert(result.hasOwnProperty("m.newer"), "Should use last record in the file");
            assert.eq(result["m.newer"].value, 2);
            assert(!result.hasOwnProperty("m.older"), "Should not use earlier records");
        });

        it("selects the valid record when some files are empty", function () {
            const dir = createTestDir("some_empty");

            writeFile(dir + "/empty1-metrics.jsonl", "");
            writeMetricsFile(dir, "valid-metrics.jsonl", [
                makeRecord([makeSumMetric("m.valid", [makeDataPoint(42, kTime2Ns)])]),
            ]);
            writeFile(dir + "/empty2-metrics.jsonl", "");

            const result = getLatestMetrics(dir);
            assert.neq(result, null);
            assert.eq(result["m.valid"].value, 42);
        });

        it("selects the latest record when competing with empty files", function () {
            const dir = createTestDir("compete_empty");

            writeFile(dir + "/empty-metrics.jsonl", "");
            writeMetricsFile(dir, "early-metrics.jsonl", [
                makeRecord([makeSumMetric("m.early", [makeDataPoint(1, kTime1Ns)])]),
            ]);
            writeMetricsFile(dir, "late-metrics.jsonl", [
                makeRecord([makeSumMetric("m.late", [makeDataPoint(2, kTime3Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.neq(result, null);
            assert(result.hasOwnProperty("m.late"));
            assert.eq(result["m.late"].value, 2);
        });
    });

    describe("sum metrics", function () {
        it("returns the value of a single sum data point", function () {
            const dir = createTestDir("sum_single");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeSumMetric("counter", [makeDataPoint(100, kTime1Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["counter"].value, 100);
        });

        it("accumulates values across multiple sum data points", function () {
            const dir = createTestDir("sum_multi");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([
                    makeSumMetric("counter", [
                        makeDataPoint(10, kTime1Ns),
                        makeDataPoint(25, kTime1Ns),
                        makeDataPoint(5, kTime1Ns),
                    ]),
                ]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["counter"].value, 40);
        });

        it("handles asInt values", function () {
            const dir = createTestDir("sum_int");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeSumMetric("counter", [makeDataPoint(7, kTime1Ns, true)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["counter"].value, 7);
        });

        it("handles asDouble values", function () {
            const dir = createTestDir("sum_double");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeSumMetric("counter", [makeDataPoint(3.14, kTime1Ns, false)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["counter"].value, 3.14);
        });
    });

    describe("gauge metrics", function () {
        it("returns the value of a single gauge data point", function () {
            const dir = createTestDir("gauge_single");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeGaugeMetric("active_conns", [makeDataPoint(5, kTime1Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["active_conns"].value, 5);
        });

        it("accumulates values across multiple gauge data points", function () {
            const dir = createTestDir("gauge_multi");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeGaugeMetric("active_conns", [makeDataPoint(3, kTime1Ns), makeDataPoint(7, kTime1Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["active_conns"].value, 10);
        });
    });

    describe("histogram metrics", function () {
        it("returns the histogram data point fields directly", function () {
            const dir = createTestDir("hist_single");

            const histDP = makeHistogramDataPoint(kTime1Ns, {
                count: 5,
                sum: 120,
                min: 2,
                max: 50,
                bucketCounts: [1, 2, 1, 1, 0],
                explicitBounds: [5, 10, 25, 50],
            });

            writeMetricsFile(dir, "test-metrics.jsonl", [makeRecord([makeHistogramMetric("duration", [histDP])])]);

            const result = getLatestMetrics(dir);
            assert.neq(result, null);
            assert.eq(result["duration"].count, "5");
            assert.eq(result["duration"].sum, 120);
            assert.eq(result["duration"].min, 2);
            assert.eq(result["duration"].max, 50);
            assert.eq(result["duration"].bucketCounts.length, 5);
            assert.eq(result["duration"].explicitBounds.length, 4);
        });
    });

    describe("mixed metric types", function () {
        it("handles sum, gauge, and histogram metrics in the same record", function () {
            const dir = createTestDir("mixed_all");

            const histDP = makeHistogramDataPoint(kTime1Ns, {
                count: 3,
                sum: 60,
                min: 10,
                max: 30,
                bucketCounts: [1, 1, 1],
                explicitBounds: [10, 25],
            });

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([
                    makeSumMetric("total_conns", [makeDataPoint(100, kTime1Ns)]),
                    makeGaugeMetric("active_conns", [makeDataPoint(5, kTime1Ns)]),
                    makeHistogramMetric("latency", [histDP]),
                ]),
            ]);

            const result = getLatestMetrics(dir);
            assert.neq(result, null);
            assert.eq(result["total_conns"].value, 100);
            assert.eq(result["active_conns"].value, 5);
            assert.eq(result["latency"].count, "3");
            assert.eq(result["latency"].sum, 60);
        });

        it("handles multiple metrics of the same type", function () {
            const dir = createTestDir("multi_same");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([
                    makeSumMetric("counter_a", [makeDataPoint(10, kTime1Ns)]),
                    makeSumMetric("counter_b", [makeDataPoint(20, kTime1Ns)]),
                    makeGaugeMetric("gauge_a", [makeDataPoint(3, kTime1Ns)]),
                    makeGaugeMetric("gauge_b", [makeDataPoint(7, kTime1Ns)]),
                ]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result["counter_a"].value, 10);
            assert.eq(result["counter_b"].value, 20);
            assert.eq(result["gauge_a"].value, 3);
            assert.eq(result["gauge_b"].value, 7);
        });
    });

    describe("time property", function () {
        it("sets the time property to the latest data point time in milliseconds", function () {
            const dir = createTestDir("time_basic");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeSumMetric("m", [makeDataPoint(1, kTime1Ns)])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result.time, kTime1Ms);
        });

        it("picks the max time across data points from different metrics", function () {
            const dir = createTestDir("time_max");

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([
                    makeSumMetric("m1", [makeDataPoint(1, kTime1Ns)]),
                    makeSumMetric("m2", [makeDataPoint(2, kTime3Ns)]),
                    makeGaugeMetric("m3", [makeDataPoint(3, kTime2Ns)]),
                ]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result.time, kTime3Ms);
        });

        it("includes histogram data point times when computing the max", function () {
            const dir = createTestDir("time_hist");

            const histDP = makeHistogramDataPoint(kTime3Ns, {
                count: 1,
                sum: 10,
                min: 10,
                max: 10,
                bucketCounts: [1],
                explicitBounds: [],
            });

            writeMetricsFile(dir, "test-metrics.jsonl", [
                makeRecord([makeSumMetric("m1", [makeDataPoint(1, kTime1Ns)]), makeHistogramMetric("h1", [histDP])]),
            ]);

            const result = getLatestMetrics(dir);
            assert.eq(result.time, kTime3Ms);
        });
    });
});

describe("extractPrometheusMetricIntValue", function () {
    it("returns the integer value for a metric with empty labels", function () {
        assert.eq(
            extractPrometheusMetricIntValue(
                /*metricsText=*/ "network_connections_processed_total{} 42 123456\n",
                /*metricName=*/ "network.connections_processed",
            ),
            42,
        );
    });

    it("returns the integer value for a metric with a scope label", function () {
        assert.eq(
            extractPrometheusMetricIntValue(
                /*metricsText=*/ 'network_connections_processed_total{otel_scope_name="mongodb"} 99 123456\n',
                /*metricName=*/ "network.connections_processed",
            ),
            99,
        );
    });

    it("replaces dots with underscores in the metric name for matching", function () {
        assert.eq(
            extractPrometheusMetricIntValue(/*metricsText=*/ "a_b_c_total{} 7 123456\n", /*metricName=*/ "a.b.c"),
            7,
        );
    });

    it("returns null when the metric is not present in the text", function () {
        assert.eq(
            extractPrometheusMetricIntValue(
                /*metricsText=*/ "some_other_metric_total{} 10 123456\n",
                /*metricName=*/ "network.connections_processed",
            ),
            null,
        );
    });

    it("returns null for null metricsText", function () {
        assert.eq(
            extractPrometheusMetricIntValue(/*metricsText=*/ null, /*metricName=*/ "network.connections_processed"),
            null,
        );
    });

    it("returns null for empty metricsText", function () {
        assert.eq(
            extractPrometheusMetricIntValue(/*metricsText=*/ "", /*metricName=*/ "network.connections_processed"),
            null,
        );
    });

    it("extracts the correct metric when multiple metrics are present", function () {
        const text =
            [
                'network_bytes_sent_bytes{otel_scope_name="mongodb"} 1024 123456',
                'network_connections_processed_total{otel_scope_name="mongodb"} 50 123456',
                'db_operations_total{otel_scope_name="mongodb"} 200 123456',
            ].join("\n") + "\n";

        assert.eq(extractPrometheusMetricIntValue(text, /*metricName=*/ "network.connections_processed"), 50);
        assert.eq(extractPrometheusMetricIntValue(text, /*metricName=*/ "network.bytes_sent"), 1024);
        assert.eq(extractPrometheusMetricIntValue(text, /*metricName=*/ "db.operations"), 200);
    });

    it("handles Prometheus comment and TYPE lines without false matches", function () {
        const text =
            [
                "# HELP network_connections_processed_total Total connections processed",
                "# TYPE network_connections_processed_total counter",
                'network_connections_processed_total{otel_scope_name="mongodb"} 77 123456',
            ].join("\n") + "\n";

        assert.eq(extractPrometheusMetricIntValue(text, /*metricName=*/ "network.connections_processed"), 77);
    });

    it("matches metrics with different unit suffixes", function () {
        assert.eq(
            extractPrometheusMetricIntValue(
                /*metricsText=*/ "request_duration_seconds{} 15 123456\n",
                /*metricName=*/ "request.duration",
            ),
            15,
        );
    });

    it("ignores leading and trailing whitespace", function () {
        assert.eq(
            extractPrometheusMetricIntValue(
                /*metricsText=*/ "  \t network_connections_processed_total{} 33 123456  \t \n",
                /*metricName=*/ "network.connections_processed",
            ),
            33,
        );
    });
});

describe("extractPrometheusMetricTime", function () {
    it("returns the timestamp from a target_info line with empty braces", function () {
        assert.eq(extractPrometheusMetricTime(/*metricsText=*/ "target_info{} 1 1700000000000\n"), 1700000000000);
    });

    it("returns the timestamp from a target_info line with attributes", function () {
        assert.eq(
            extractPrometheusMetricTime(
                /*metricsText=*/ 'target_info{service_name="mongodb",service_instance_id="abc123"} 1 1700000000000\n',
            ),
            1700000000000,
        );
    });

    it("returns null for null metricsText", function () {
        assert.eq(extractPrometheusMetricTime(/*metricsText=*/ null), null);
    });

    it("returns null for empty metricsText", function () {
        assert.eq(extractPrometheusMetricTime(/*metricsText=*/ ""), null);
    });

    it("returns null when no target_info line is present", function () {
        assert.eq(
            extractPrometheusMetricTime(
                /*metricsText=*/ 'network_connections_processed_total{otel_scope_name="mongodb"} 50 123456\n',
            ),
            null,
        );
    });

    it("extracts the timestamp when target_info is not the first line", function () {
        const text =
            [
                "# HELP target_info Target metadata",
                "# TYPE target_info gauge",
                'network_connections_processed_total{otel_scope_name="mongodb"} 50 123456',
                'target_info{service_name="mongodb"} 1 1700000000000',
            ].join("\n") + "\n";

        assert.eq(extractPrometheusMetricTime(text), 1700000000000);
    });

    it("ignores the value and only returns the timestamp", function () {
        assert.eq(extractPrometheusMetricTime(/*metricsText=*/ "target_info{} 99 1700000055555\n"), 1700000055555);
    });
});
