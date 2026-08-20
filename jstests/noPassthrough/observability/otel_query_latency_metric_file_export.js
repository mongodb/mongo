/**
 * Tests that the serverStatus.queryLatencies.<strategy> OTel histograms are wired up and
 * exported via the JSONL file exporter. Attribution and the one-observation-per-query lifecycle
 * are covered by query_latencies_by_plan_strategy.js.
 *
 * Unlike opLatencies, there is one metric per strategy rather than one keyed by strategy, so these
 * histograms carry no attributes and the count is read without an attribute filter.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertHistogramMetricIncreases,
    getLatestMetrics,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";

function assertQueryLatencyIncreases(metricsDir, strategy, fn) {
    assertHistogramMetricIncreases({
        metricsDir,
        metricName: `mongodb.serverStatus.queryLatencies.${strategy}`,
        fn,
    });
}

describe("OTel serverStatus.queryLatencies histogram file export", function () {
    before(function () {
        const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
        this.metricsDir = metricsDir;

        this.mongod = MongoRunner.runMongod({
            setParameter: {
                ...otelParams,
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
                featureFlagCostBasedRanker: true,
                internalQueryFrameworkControl: "forceClassicEngine",
                internalQueryPlanRanker: "mixed",
            },
        });
        assert.neq(this.mongod, null, "mongod failed to start");

        this.db = this.mongod.getDB("test");

        // No index: every query over this collection is a single-plan collscan.
        this.singlePlanColl = this.db.single_plan;
        assert.commandWorked(
            this.singlePlanColl.insertMany(Array.from({length: 100}, (_, i) => ({a: i}))),
        );

        // Two competing indexes: a matching query goes through the multi-planner, and once its
        // plan-cache entry is active an identical query reuses it.
        this.multiPlanColl = this.db.multi_plan;
        assert.commandWorked(this.multiPlanColl.createIndexes([{a: 1}, {b: 1}]));
        assert.commandWorked(
            this.multiPlanColl.insertMany(Array.from({length: 200}, (_, i) => ({a: i % 10, b: i}))),
        );

        assert.soon(
            () => getLatestMetrics(metricsDir) !== null,
            "No initial metrics export",
            30000,
            500,
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("exports singlePlan query latencies", function () {
        assertQueryLatencyIncreases(this.metricsDir, "singlePlan", () =>
            assert.eq(this.singlePlanColl.find({a: 42}).itcount(), 1),
        );
    });

    it("exports multiPlanner query latencies", function () {
        assert.commandWorked(this.db.runCommand({planCacheClear: this.multiPlanColl.getName()}));
        assertQueryLatencyIncreases(this.metricsDir, "multiPlanner", () =>
            assert.gte(this.multiPlanColl.find({a: 5, b: {$gte: 0}}).itcount(), 1),
        );
    });

    it("exports cachedPlan query latencies", function () {
        // Run the query enough times to move its plan-cache entry from inactive to active.
        const query = {a: 5, b: {$gte: 0}};
        for (let i = 0; i < 5; i++) {
            this.multiPlanColl.find(query).itcount();
        }
        assertQueryLatencyIncreases(this.metricsDir, "cachedPlan", () =>
            assert.gte(this.multiPlanColl.find(query).itcount(), 1),
        );
    });

    it("exports costBased query latencies", function () {
        // Two indexes, no matching documents, and a collection large enough that the multi-planner
        // trial hits its work budget before EOF, so CBR picks the winner.
        const coll = this.db.cbr_plan;
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
        assert.commandWorked(
            coll.insertMany(Array.from({length: 5001}, (_, i) => ({a: i, b: i, c: i}))),
        );
        assert.commandWorked(this.db.runCommand({planCacheClear: coll.getName()}));

        assertQueryLatencyIncreases(this.metricsDir, "costBased", () =>
            assert.eq(coll.find({a: {$gt: 0}, b: {$gt: 0}, c: -1}).itcount(), 0),
        );
    });
});
