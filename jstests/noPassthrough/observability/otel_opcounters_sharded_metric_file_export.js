/**
 * Tests that OpCounters metrics are independently exported via the OTel JSONL file exporter on
 * both mongos and each shard in a sharded cluster.
 *
 * Operations routed through mongos increment opcounters on mongos; the same operations forwarded
 * to a shard also increment opcounters on that shard. Both metric streams are verified.
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
 * @tags: [requires_otel_build, requires_sharding]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    createMetricsDirectory,
    getLatestMetrics,
    waitForMetric,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

const kOtelParams = {
    openTelemetryExportIntervalMillis: 500,
    openTelemetryExportTimeoutMillis: 200,
};

/**
 * Returns the current value of a metric from the latest export, or 0 if not yet present.
 */
function currentValue(metricsDir, metricName) {
    return getLatestMetrics(metricsDir)?.[metricName]?.value ?? 0;
}

describe("OTel opcounters metric file export in a sharded cluster", function () {
    before(function () {
        this.mongosMetricsDir = createMetricsDirectory(jsTestName() + "_mongos");
        this.shardMetricsDir = createMetricsDirectory(jsTestName() + "_shard");

        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            rs: {nodes: 1},
            mongosOptions: {
                setParameter: {
                    ...kOtelParams,
                    openTelemetryMetricsDirectory: this.mongosMetricsDir,
                },
            },
            rsOptions: {
                setParameter: {
                    ...kOtelParams,
                    openTelemetryMetricsDirectory: this.shardMetricsDir,
                },
            },
        });

        this.db = this.st.s.getDB("test");
        this.coll = this.db.getCollection(jsTestName());

        // Ensure at least one export cycle has completed on both components before tests run.
        assert.commandWorked(this.db.runCommand({ping: 1}));
        assert.soon(
            () => getLatestMetrics(this.mongosMetricsDir) !== null,
            "No initial mongos metrics export",
            30000,
            500,
        );
        assert.soon(
            () => getLatestMetrics(this.shardMetricsDir) !== null,
            "No initial shard metrics export",
            30000,
            500,
        );
    });

    after(function () {
        this.st.stop();
    });

    it("exports opcounters.inserts on both mongos and shard", function () {
        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.inserts");
        const shardInitial = currentValue(this.shardMetricsDir, "opcounters.inserts");

        assert.commandWorked(this.coll.insert({a: 1}));
        assert.commandWorked(this.coll.insert([{b: 2}, {c: 3}]));

        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.inserts",
            minValue: mongosInitial + 3,
            afterDate: start,
        });
        waitForMetric({
            metricsDir: this.shardMetricsDir,
            metricName: "opcounters.inserts",
            minValue: shardInitial + 3,
            afterDate: start,
        });
    });

    it("exports opcounters.queries on both mongos and shard", function () {
        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.queries");
        const shardInitial = currentValue(this.shardMetricsDir, "opcounters.queries");

        this.coll.find({}).toArray();

        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.queries",
            minValue: mongosInitial + 1,
            afterDate: start,
        });
        waitForMetric({
            metricsDir: this.shardMetricsDir,
            metricName: "opcounters.queries",
            minValue: shardInitial + 1,
            afterDate: start,
        });
    });

    it("exports opcounters.updates on both mongos and shard", function () {
        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.updates");
        const shardInitial = currentValue(this.shardMetricsDir, "opcounters.updates");

        assert.commandWorked(this.coll.update({a: 1}, {$set: {a: 99}}));

        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.updates",
            minValue: mongosInitial + 1,
            afterDate: start,
        });
        waitForMetric({
            metricsDir: this.shardMetricsDir,
            metricName: "opcounters.updates",
            minValue: shardInitial + 1,
            afterDate: start,
        });
    });

    it("exports opcounters.deletes on both mongos and shard", function () {
        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.deletes");
        const shardInitial = currentValue(this.shardMetricsDir, "opcounters.deletes");

        assert.commandWorked(this.coll.remove({a: 99}));

        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.deletes",
            minValue: mongosInitial + 1,
            afterDate: start,
        });
        waitForMetric({
            metricsDir: this.shardMetricsDir,
            metricName: "opcounters.deletes",
            minValue: shardInitial + 1,
            afterDate: start,
        });
    });

    it("exports opcounters.get_mores on both mongos and shard", function () {
        // Insert enough documents to force a getMore with a small batchSize.
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(this.coll.insert({x: i}));
        }

        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.get_mores");
        const shardInitial = currentValue(this.shardMetricsDir, "opcounters.get_mores");

        this.coll
            .find({x: {$exists: true}})
            .batchSize(2)
            .toArray();

        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.get_mores",
            minValue: mongosInitial + 1,
            afterDate: start,
        });
        waitForMetric({
            metricsDir: this.shardMetricsDir,
            metricName: "opcounters.get_mores",
            minValue: shardInitial + 1,
            afterDate: start,
        });
    });

    it("exports opcounters.aggregates on both mongos and shard", function () {
        // opcounters.aggregates increments once per top-level aggregate call only.
        // 'find' increments 'queries' but NOT 'aggregates'; the two are fully exclusive.
        // Neither aggregate nor find increments 'commands'.
        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.aggregates");
        const shardInitial = currentValue(this.shardMetricsDir, "opcounters.aggregates");

        this.coll.aggregate([{$match: {}}]).toArray();
        this.coll.aggregate([{$match: {}}, {$count: "n"}]).toArray();

        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.aggregates",
            minValue: mongosInitial + 2,
            afterDate: start,
        });
        waitForMetric({
            metricsDir: this.shardMetricsDir,
            metricName: "opcounters.aggregates",
            minValue: shardInitial + 2,
            afterDate: start,
        });
    });

    it("exports opcounters.commands on both mongos and shard", function () {
        // opcounters.commands increments for recognized commands that have no specific counter of
        // their own (e.g. ping, serverStatus). 'aggregate', 'find', getMore, and write ops all
        // suppress this counter and increment their own specific counters instead.
        const start = new Date();
        const mongosInitial = currentValue(this.mongosMetricsDir, "opcounters.commands");
        // No assertion/expectation on the shard.

        assert.commandWorked(this.db.runCommand({ping: 1}));

        // ping is handled by mongos directly; the shard receives it via internal heartbeats
        // and other background commands, so we only assert mongos here.
        waitForMetric({
            metricsDir: this.mongosMetricsDir,
            metricName: "opcounters.commands",
            minValue: mongosInitial + 1,
            afterDate: start,
        });

        // Verify the shard's command counter is also being exported (value may vary due to
        // background replication commands, so just check the metric is present and non-zero).
        assert.soon(
            () => currentValue(this.shardMetricsDir, "opcounters.commands") > 0,
            "opcounters.commands should be present and non-zero on shard",
            30000,
            300,
        );
    });
});
