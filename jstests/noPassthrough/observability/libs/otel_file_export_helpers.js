/**
 * Finds metrics files in the given directory created after the specified timestamp.
 * @param {string} directory - The directory path to search in.
 * @param {Date} afterDate - Only return files created after this Date. If not provided, all metrics files are returned.
 * @returns {Array<Object>} An array of file objects (from listFiles()) whose names end with "-metrics.jsonl" and were
 *     created after the specified timestamp. Each file object has a 'name' property containing the full file path.
 */
export function findMetricsFiles(directory, afterDate = new Date(0)) {
    const files = listFiles(directory);
    return files.filter(function (file) {
        if (!file.name.endsWith("-metrics.jsonl")) {
            return false;
        }
        if (file.lastModified < afterDate.getTime()) {
            jsTest.log.info(
                `Skipping metric file ${file.name} because it was modified ` +
                    `${new Date(file.lastModified)} which is before ${afterDate}`,
            );
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
export function readJsonlFile(filePath) {
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
        try {
            records.push(JSON.parse(line));
        } catch (e) {
            // JSON parse can fail if we read the file while it's being written to.
            // Return empty array to signal the caller should retry.
            jsTest.log.info("Failed to parse JSONL line (file may be mid-write): " + e.message);
            return [];
        }
    }
    return records;
}

/**
 * Finds the latest metric record with the given name in the OTel OTLP JSON export data.
 * Iterates in reverse order since later records in JSONL are more recent.
 * @param {Array<Object>} records - Array of OTel OTLP JSON records (from readJsonlFile()).
 * @param {string} name - The metric name to search for (e.g., "connections_processed").
 * @returns {Object|null} The metric object if found, in the following format:
 *     {
 *         "description": "Total number of ingress connections processed (accepted or rejected)",
 *         "name": "connections_processed",
 *         "sum": {
 *             "aggregationTemporality": 2,
 *             "dataPoints": [
 *                 {
 *                     "asInt": "4",
 *                     "startTimeUnixNano": "1767733224785580274",
 *                     "timeUnixNano": "1767733225285751085"
 *                 }
 *             ],
 *             "isMonotonic": true
 *         },
 *         "unit": "connections"
 *     }
 *     Returns null if no metric with the given name is found.
 */
export function findMetric(records, name) {
    for (let i = records.length - 1; i >= 0; i--) {
        const record = records[i];
        // OTel OTLP JSON format has resourceMetrics array
        if (!record.resourceMetrics) {
            continue;
        }
        for (const resourceMetric of record.resourceMetrics) {
            if (!resourceMetric.scopeMetrics) {
                continue;
            }
            for (const scopeMetric of resourceMetric.scopeMetrics) {
                if (!scopeMetric.metrics) {
                    continue;
                }
                for (const metric of scopeMetric.metrics) {
                    if (metric.name === name) {
                        return metric;
                    }
                }
            }
        }
    }
    return null;
}
