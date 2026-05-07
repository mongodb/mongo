/**
 * Tests that the main OpCounters are exported via the OTel JSONL file exporter and that their
 * values increase to reflect actual operations performed against mongod.
 *
 * Counter semantics
 * -----------------
 * All operation-specific counters are mutually exclusive — each command increments exactly one:
 *
 *   aggregate call        →  aggregates++   (queries and commands are NOT incremented)
 *   find call             →  queries++      (aggregates and commands are NOT incremented)
 *   getMore call          →  get_mores++    (commands is NOT incremented)
 *   insert/update/delete  →  their own++   (commands is NOT incremented)
 *   ping, serverStatus, etc.  →  commands++ (no specific counter for these)
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getLatestMetrics,
    otelFileExportParams,
    waitForMetric,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

describe("OTel opcounters metric file export", function () {
    before(function () {
        const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
        this.metricsDir = metricsDir;

        this.mongod = MongoRunner.runMongod({
            setParameter: {
                ...otelParams,
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
            },
        });

        this.db = this.mongod.getDB("test");
        this.coll = this.db.getCollection(jsTestName());

        // Warm up: ensure at least one export has occurred before any test reads metrics.
        assert.soon(() => getLatestMetrics(metricsDir) !== null, "No initial metrics export", 30000, 500);
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("increments opcounters.inserts on insert operations", function () {
        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.inserts"]?.value ?? 0;

        assert.commandWorked(this.coll.insert({a: 1}));
        assert.commandWorked(this.coll.insert([{b: 2}, {c: 3}]));

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.inserts",
            minValue: initial + 3,
            afterDate: start,
        });
    });

    it("increments opcounters.queries on find operations", function () {
        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.queries"]?.value ?? 0;

        this.coll.find({}).toArray();

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.queries",
            minValue: initial + 1,
            afterDate: start,
        });
    });

    it("increments opcounters.updates on update operations", function () {
        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.updates"]?.value ?? 0;

        assert.commandWorked(this.coll.update({a: 1}, {$set: {a: 99}}));

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.updates",
            minValue: initial + 1,
            afterDate: start,
        });
    });

    it("increments opcounters.deletes on remove operations", function () {
        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.deletes"]?.value ?? 0;

        assert.commandWorked(this.coll.remove({a: 99}));

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.deletes",
            minValue: initial + 1,
            afterDate: start,
        });
    });

    it("increments opcounters.get_mores on cursor getMore operations", function () {
        // Insert enough documents to force a getMore with a small batchSize.
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(this.coll.insert({x: i}));
        }

        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.get_mores"]?.value ?? 0;

        this.coll
            .find({x: {$exists: true}})
            .batchSize(2)
            .toArray();

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.get_mores",
            minValue: initial + 1,
            afterDate: start,
        });
    });

    it("increments opcounters.commands on command operations", function () {
        // opcounters.commands increments for recognized commands that have no specific counter of
        // their own (e.g. ping, serverStatus). 'aggregate', 'find', getMore, and write ops all
        // suppress this counter and increment their own specific counters instead.
        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.commands"]?.value ?? 0;

        assert.commandWorked(this.db.runCommand({ping: 1}));

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.commands",
            minValue: initial + 1,
            afterDate: start,
        });
    });

    it("increments opcounters.aggregates on aggregate operations", function () {
        // opcounters.aggregates increments once per top-level aggregate call only.
        // 'find' increments 'queries' but NOT 'aggregates'; the two are fully exclusive.
        // Neither aggregate nor find increments 'commands'.
        const start = new Date();
        const initial = getLatestMetrics(this.metricsDir)?.["opcounters.aggregates"]?.value ?? 0;

        this.coll.aggregate([{$match: {}}]).toArray();
        this.coll.aggregate([{$match: {}}, {$count: "n"}]).toArray();

        waitForMetric({
            metricsDir: this.metricsDir,
            metricName: "opcounters.aggregates",
            minValue: initial + 2,
            afterDate: start,
        });
    });
});
