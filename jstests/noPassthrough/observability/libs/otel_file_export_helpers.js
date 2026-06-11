/**
 * Returns true if this build was compiled with OpenTelemetry support.
 * @returns {boolean}
 */
export function buildSupportsOtel() {
    return !!getBuildInfo().opentelemetry;
}

/**
 * Finds metrics files in the given directory.
 * @param {string} directory - The directory path to search in.
 * @param {string} fileSuffix - The suffix of the metrics files to search for. Defaults to "-metrics.jsonl" (the suffix
 *    for the opentelemetry metrics files in JSONL format).
 * @returns {Array<Object>} An array of file objects (from listFiles()) whose names end with `fileSuffix`. Each file
 *     object has a 'name' property containing the full file path.
 */
export function findMetricsFiles(directory, fileSuffix = "-metrics.jsonl") {
    const files = listFiles(directory);
    return files.filter(function (file) {
        if (!file.name.endsWith(fileSuffix)) {
            return false;
        }
        return true;
    });
}

/**
 * Reads and parses a JSONL (JSON Lines) file.
 * @param {string} filePath - The path to the JSONL file.
 * @returns {Array<Object>} An array of parsed JSON objects, one per line in the file.
 *     Returns an empty array if the file is empty, doesn't exist, or if parsing fails
 *     (which can happen if the file is read mid-write).
 */
function readJsonlFile(filePath) {
    const content = cat(filePath);
    if (!content || content.trim() === "") {
        return [];
    }
    const lines = content.trim().split("\n");
    const records = [];
    for (const line of lines) {
        if (!line.trim()) {
            continue;
        }
        let parsedLine;
        try {
            parsedLine = JSON.parse(line);
        } catch (e) {
            // The last record is often cut off due to being read mid-write, but previous records may be valid.
            return records;
        }
        records.push(parsedLine);
    }
    return records;
}

function safeConvertToNumber(value) {
    // Comparing to the MIN and MAX safe integers guarantees that the resulting value is precise, i.e., for numbers
    // outside this range, the result could be rounded to a slightly higher or lower value.
    assert(value >= Number.MIN_SAFE_INTEGER);
    assert(value <= Number.MAX_SAFE_INTEGER);
    return Number(value);
}

/**
 * Finds the latest time from all points in the given record.
 * @param {Object} record - The record to search in.
 * @returns {number} The latest time from all points in the record in milliseconds. Returns 0 if no points are found.
 */
function latestTime(record) {
    let allPoints = [];
    for (const resourceMetric of record?.resourceMetrics ?? []) {
        for (const scopeMetric of resourceMetric.scopeMetrics ?? []) {
            for (const metric of scopeMetric.metrics ?? []) {
                allPoints.push(...(metric.sum?.dataPoints ?? []));
                allPoints.push(...(metric.gauge?.dataPoints ?? []));
                allPoints.push(...(metric.histogram?.dataPoints ?? []));
            }
        }
    }
    return allPoints.length > 0
        ? Math.max(...allPoints.map((point) => safeConvertToNumber(BigInt(point.timeUnixNano) / 1_000_000n)))
        : 0;
}

/**
 * Returns the most recent raw OTLP JSON record from the metrics directory, or null if none exist.
 */
export function getLatestRawRecord(directory) {
    const files = findMetricsFiles(directory);
    const records = files
        .map((file) => {
            const record = readJsonlFile(file.name);
            if (record.length === 0) {
                jsTest.log.info(`No records found in file ${file.name}`);
            }
            return record;
        })
        .filter((record) => record.length > 0)
        .map((records) => records.at(-1));
    if (records.length === 0) {
        return null;
    }
    return records.reduce((latest, curRecord) => {
        return latestTime(latest) > latestTime(curRecord) ? latest : curRecord;
    }, records.at(0));
}

/**
 * Gets the most recent metrics from the given directory.
 * @param {string} directory - The directory path to search in.
 * @returns {Object} An object with the metric name as the key and the metric value as the value, or null if no metrics
 *  are found. The value is an object with the "value" property containing the metric value for counters and gauges.
 *  Also contains a special "time" property containing the latest time from all points in the record in milliseconds.
 *  Example:
 *  {
 *     "time": 1771619346,
 *     // A counter or gauge metric.
 *     "network.connections_processed": {
 *          "value": 100
 *      },
 *      // A histogram metric.
 *      "network.tls_handshake_latency": {
 *          "count": 100,
 *          "sum": 10000,
 *          "min": 100,
 *          "max": 10000,
 *          "bucketCounts":   [800, 900, 800, 400, 300, 200, 150, 125, 110, 101],
 *          "explicitBounds": [100, 200, 300, 400, 500, 600, 700, 800, 900, 1000]
 *      }
 *      ...
 *  }
 */
export function getLatestMetrics(directory) {
    const record = getLatestRawRecord(directory);
    if (!record) {
        return null;
    }

    let metrics = [];
    for (const resourceMetric of record.resourceMetrics ?? []) {
        for (const scopeMetric of resourceMetric.scopeMetrics ?? []) {
            for (const metric of scopeMetric.metrics ?? []) {
                metrics.push(metric);
            }
        }
    }
    let result = Object.fromEntries(
        metrics.map((metric) => {
            let result = {};
            for (const dataPoint of (metric.sum?.dataPoints ?? []).concat(metric.gauge?.dataPoints ?? [])) {
                if (dataPoint.asInt) {
                    result["value"] ??= 0;
                    result["value"] += Number(dataPoint.asInt);
                } else {
                    result["value"] ??= 0;
                    result["value"] += Number(dataPoint.asDouble);
                }
            }
            for (const dataPoint of metric.histogram?.dataPoints ?? []) {
                // There should only be one data point for the histogram.
                result = dataPoint;
            }
            return [metric.name, result];
        }),
    );
    result.time = latestTime(record);
    return result;
}

/**
 * Returns the histogram data point count for a specific metric name and attribute key/value pair
 * from the latest metrics snapshot, or 0 if not found. Useful for histograms with multiple data
 * points keyed by an attribute (e.g. op_type: "read").
 */
export function getHistogramCount(metricsDir, metricName, attrKey, attrValue) {
    const record = getLatestRawRecord(metricsDir);
    if (!record) return 0;
    for (const resourceMetric of record.resourceMetrics ?? []) {
        for (const scopeMetric of resourceMetric.scopeMetrics ?? []) {
            for (const metric of scopeMetric.metrics ?? []) {
                if (metric.name !== metricName) continue;
                for (const dp of metric.histogram?.dataPoints ?? []) {
                    const match = (dp.attributes ?? []).some(
                        (a) => a.key === attrKey && a.value?.stringValue === attrValue,
                    );
                    if (match) return Number(dp.count ?? 0);
                }
            }
        }
    }
    return 0;
}

/**
 * Parses the integer value of the metric from Prometheus text format.
 * @param {string} metricsText - The text of the metrics file.
 * @param {string} metricName - The name of the metric to extract the value of.
 * @returns {number} The integer value of the metric. Returns null if the metric is not found.
 */
export function extractPrometheusMetricIntValue(metricsText, metricName) {
    // Match lines like: network_connections_processed_total{} <value>
    // `.` gets replaced with `_` when switching to Prometheus format.
    // The OTel Prometheus exporter appends a unit suffix and/or a `_total` type suffix; use
    // `(?:_[a-zA-Z]+)+` to skip one or more such groups before the label set.
    const leadingWhitespaceRe = "(?:^|\\s)+";
    const metricNameRe = metricName.replace(/\./g, "_");
    const unitSuffixRe = "(?:_[a-zA-Z]+)+";
    const scopeRe = '\\{(?:\\w+\\="\\w+")?\\}';
    const valueRe = "\\s+(\\d+)";

    const regexp = new RegExp(leadingWhitespaceRe + metricNameRe + unitSuffixRe + scopeRe + valueRe, "m");
    const match = metricsText && metricsText.match(regexp);
    if (match) {
        return parseInt(match[1], 10);
    }
    return null;
}

/**
 * Parses the time of the set of metrics from Prometheus text format.
 * @param {string} metricsText - The text of the metrics file.
 * @returns {numeric} The time of the metrics in milliseconds. Returns null no timestamp is found.
 */
export function extractPrometheusMetricTime(metricsText) {
    // Match lines like: target_info{...} <some_number> <timestamp>
    // The timestamp is the third value in the line.
    const leadingWhitespaceRe = "(?:^|\\s)+";
    const targetInfoRe = "target_info\\{[^}]*\\}";
    const numberRe = "\\s+(\\d+)";
    const timestampRe = "\\s+(\\d+)";

    const regexp = new RegExp(leadingWhitespaceRe + targetInfoRe + numberRe + timestampRe, "m");
    const match = metricsText && metricsText.match(regexp);
    if (match) {
        return parseInt(match[2], 10);
    }
    return null;
}

/**
 * Generates the name for a metrics directory for the given test name and ensures the directory exists. The name is
 * randomized so the same testName will provide different results on different calls.
 * @param {string} testName - The name of the test.
 * @returns {string} The path to the metrics directory.
 */
export function createMetricsDirectory(testName) {
    if (!Random.isInitialized()) {
        Random.setRandomSeed();
    }
    const metricsDir = MongoRunner.toRealPath(`${testName}_otel_metrics_${Random.randInt(1000000)}`);
    assert(mkdir(metricsDir), `Failed to create metrics directory: ${metricsDir}`);
    return metricsDir;
}

/**
 * Creates a metrics directory and returns the setParameter entries needed to enable OTEL file export for testing.
 *
 * @param {string} testName - The name of the test.
 * @returns {{metricsDir: string, otelParams: Object}} An object with:
 *   - metricsDir: The path to the created metrics directory, for use when reading metrics.
 *   - otelParams: The setParameter entries to spread into a node's setParameter config.
 */
export function otelFileExportParams(testName) {
    const metricsDir = createMetricsDirectory(testName);
    return {
        metricsDir,
        otelParams: {
            openTelemetryMetricsDirectory: metricsDir,
        },
    };
}

/**
 * Waits until the named metric in the most-recently-exported file is >= minValue, reading only
 * files written after afterDate.
 */
export function waitForMetric({metricsDir, metricName, minValue, afterDate}) {
    assert.soon(
        () => {
            const metrics = getLatestMetrics(metricsDir);
            return metrics && metrics.time > afterDate.getTime() && (metrics[metricName]?.value ?? 0) >= minValue;
        },
        `Expected ${metricName} >= ${minValue} in ${metricsDir}`,
        30000,
        300,
    );
}

/**
 * Asserts that the data point count for `metricName` with attribute `attrKey=attrValue` increases
 * by at least `minIncrease` after `fn` runs.
 */
export function assertHistogramMetricIncreases({metricsDir, metricName, attrKey, attrValue, minIncrease = 1, fn}) {
    const baseline = getHistogramCount(metricsDir, metricName, attrKey, attrValue);
    fn();
    assert.soon(
        () => getHistogramCount(metricsDir, metricName, attrKey, attrValue) >= baseline + minIncrease,
        `Expected ${metricName} ${attrKey}=${attrValue} count to increase by at least ${minIncrease} from ${baseline}`,
        30000,
        500,
    );
}
