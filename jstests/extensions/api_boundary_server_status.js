/**
 * Tests that serverStatus tracks and reports extension successes and failures.
 *
 * This test verifies that:
 * 1. extension.extensionSuccesses increases when extension stages execute successfully
 * 2. extension.extensionFailures increases when extension stages fail (e.g., via $assert)
 *
 * In sharded environments, extension stages may run on mongos or on shards depending on
 * pipeline position. The test aggregates metrics from all nodes to verify correct tracking.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {observeExtensionMetricsChange} from "jstests/extensions/libs/extension_metrics_helpers.js";

// The 'serverStatus' command is unreliable in test suites with multiple mongos processes given that
// each node has its own metrics. The assertions here would not hold up if run against multiple
// mongos.
TestData.pinToSingleMongos = true;

/**
 * Helper to get an extension server status metric from a specific node.
 */
function getExtensionMetricFromNode(conn, metric) {
    const serverStatus = conn.getDB("admin").runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);
    return serverStatus.metrics.extension[metric];
}

/**
 * Gets the sum of extension metrics from all relevant nodes.
 * In a sharded cluster, this includes mongos and all shard primaries.
 * In a standalone/replica set, this is just the current node.
 */
function getTotalExtensionMetrics(testDb, metric) {
    let total = getExtensionMetricFromNode(testDb.getMongo(), metric);

    if (FixtureHelpers.isMongos(testDb)) {
        const primaries = FixtureHelpers.getPrimaries(testDb);
        total += primaries.reduce((sum, primary) => sum + getExtensionMetricFromNode(primary, metric), 0);
    }

    return total;
}

/**
 * Gets all relevant extension metrics aggregated across all nodes.
 * Returns an object with extensionSuccesses, extensionFailures, hostSuccesses, and hostFailures.
 */
function getTotalMetrics(testDB) {
    return {
        extensionSuccesses: getTotalExtensionMetrics(testDB, "extensionSuccesses"),
        extensionFailures: getTotalExtensionMetrics(testDB, "extensionFailures"),
        hostSuccesses: getTotalExtensionMetrics(testDB, "hostSuccesses"),
        hostFailures: getTotalExtensionMetrics(testDB, "hostFailures"),
    };
}

describe("Extension success and failure serverStatus metrics", function () {
    before(function () {
        this.coll = db[jsTestName()];
        this.coll.drop();

        this.numDocs = 20;
        const docs = [];
        for (let i = 0; i < this.numDocs; i++) {
            docs.push({_id: i, value: i * 10});
        }
        assert.commandWorked(this.coll.insertMany(docs));
    });

    after(function () {
        this.coll.drop();
    });

    it("should have non-negative initial values", function () {
        const initialMetrics = getTotalMetrics(db);
        assert.gte(initialMetrics.extensionSuccesses, 0, `extensionSuccesses should be non-negative.`);
        assert.gte(initialMetrics.extensionFailures, 0, `extensionFailures should be non-negative.`);
        assert.gte(initialMetrics.hostSuccesses, 0, `hostSuccesses should be non-negative.`);
        assert.gte(initialMetrics.hostFailures, 0, `hostFailures should be non-negative.`);
    });

    it("should increase extensionSuccesses when $testFoo runs successfully", function () {
        const result = observeExtensionMetricsChange(
            [
                {
                    operation: () => this.coll.aggregate([{$testFoo: {}}]).toArray(),
                    expectSuccess: true,
                },
            ],
            () => getTotalMetrics(db),
            ["extensionSuccesses", "extensionFailures", "hostSuccesses", "hostFailures"],
        );

        assert.eq(result.nSuccessfulOperations, 1, "Query should succeed");
        assert.gt(
            result.metricDeltas.extensionSuccesses,
            0,
            "extensionSuccesses should increase after successful extension work.",
        );
        assert.eq(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should remain stable after successful extension work.`,
        );
        // Note, hostSuccesses increases anytime the extension calls back into the host. This can be
        // an indeterminate number of times in the successful case.
        assert.gte(
            result.metricDeltas.hostSuccesses,
            0,
            `hostSuccesses should not decrease after successful host work.`,
        );
        assert.eq(result.metricDeltas.hostFailures, 0, `hostFailures should remain stable after successful host work.`);
    });

    it("should increase extensionSuccesses when $testFoo runs on mongos (after $sort)", function () {
        const result = observeExtensionMetricsChange(
            [
                {
                    operation: () => this.coll.aggregate([{$sort: {value: 1}}, {$testFoo: {}}]).toArray(),
                    expectSuccess: true,
                },
            ],
            () => getTotalMetrics(db),
            ["extensionSuccesses", "extensionFailures", "hostSuccesses", "hostFailures"],
        );

        assert.eq(result.nSuccessfulOperations, 1, "Query should succeed");
        assert.gt(
            result.metricDeltas.extensionSuccesses,
            0,
            "extensionSuccesses should increase after successful extension work.",
        );
        assert.eq(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should remain stable after successful extension work.`,
        );
        assert.gt(result.metricDeltas.hostSuccesses, 0, `hostSuccesses should increase after successful host work.`);
        assert.eq(result.metricDeltas.hostFailures, 0, `hostFailures should remain stable after successful host work.`);
    });

    it("should accumulate extensionSuccesses across multiple queries", function () {
        const numQueries = 3;
        const operations = [];
        for (let i = 0; i < numQueries; i++) {
            operations.push({
                operation: () => this.coll.aggregate([{$testFoo: {}}]).toArray(),
                expectSuccess: true,
            });
        }

        const result = observeExtensionMetricsChange(operations, () => getTotalMetrics(db), [
            "extensionSuccesses",
            "extensionFailures",
            "hostSuccesses",
            "hostFailures",
        ]);

        assert.eq(result.nSuccessfulOperations, numQueries, "All queries should succeed");
        assert.gt(
            result.metricDeltas.extensionSuccesses,
            0,
            "extensionSuccesses should accumulate across multiple queries.",
        );
        assert.eq(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should remain stable across multiple successful queries`,
        );
        // Note, hostSuccesses increases anytime the extension calls back into the host. This
        // can be an indeterminate number of times in the successful case.
        assert.gte(result.metricDeltas.hostSuccesses, 0, `hostSuccesses should not decrease across multiple queries.`);
        assert.eq(
            result.metricDeltas.hostFailures,
            0,
            `hostFailures should remain stable across multiple successful queries.`,
        );
    });

    it("extension and host failures should both increase when $assert triggers a uassert in parse phase", function () {
        const result = observeExtensionMetricsChange(
            [
                {
                    operation: () =>
                        db.runCommand({
                            aggregate: this.coll.getName(),
                            pipeline: [
                                {
                                    $assert: {errmsg: "test uassert failure", code: 11569609, assertionType: "uassert"},
                                },
                            ],
                            cursor: {},
                        }),
                    expectSuccess: false,
                },
            ],
            () => getTotalMetrics(db),
            ["extensionSuccesses", "extensionFailures", "hostSuccesses", "hostFailures"],
        );

        assert.eq(result.nFailedOperations, 1, "Operation should fail");
        assert.gte(
            result.metricDeltas.extensionSuccesses,
            0,
            `extensionSuccesses should not decrease after extension failure at parse time.`,
        );
        assert.gt(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should increase after extension failure at parse time.`,
        );
        assert.gte(
            result.metricDeltas.hostSuccesses,
            0,
            `hostSuccesses should not decrease after extension failure at parse time.`,
        );
        assert.gt(
            result.metricDeltas.hostFailures,
            0,
            `hostFailures should increase due to host triggered uassert at parse time.`,
        );
    });

    it("should increase both extensionSuccess and extensionFailures when $assert triggers a uassert in ast phase", function () {
        const result = observeExtensionMetricsChange(
            [
                {
                    operation: () =>
                        db.runCommand({
                            aggregate: this.coll.getName(),
                            pipeline: [
                                {
                                    $assert: {
                                        errmsg: "test uassert failure in ast phase",
                                        code: 11569610,
                                        assertionType: "uassert",
                                        assertInPhase: "ast",
                                    },
                                },
                            ],
                            cursor: {},
                        }),
                    expectSuccess: false,
                },
            ],
            () => getTotalMetrics(db),
            ["extensionSuccesses", "extensionFailures", "hostSuccesses", "hostFailures"],
        );

        assert.eq(result.nFailedOperations, 1, "Operation should fail");
        assert.gt(
            result.metricDeltas.extensionSuccesses,
            0,
            `extensionSuccesses should increase after extension failure in ast phase.`,
        );
        assert.gt(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should increase after extension failure in ast phase.`,
        );
        assert.gte(
            result.metricDeltas.hostSuccesses,
            0,
            `hostSuccesses should not decrease after extension failure in ast phase.`,
        );
        assert.gt(
            result.metricDeltas.hostFailures,
            0,
            `hostFailures should increase due to host triggered uassert in ast phase.`,
        );
    });

    it("should NOT increase extensionFailures for successful queries", function () {
        const result = observeExtensionMetricsChange(
            [
                {
                    operation: () => this.coll.aggregate([{$testFoo: {}}]).toArray(),
                    expectSuccess: true,
                },
            ],
            () => getTotalMetrics(db),
            ["extensionSuccesses", "extensionFailures", "hostSuccesses", "hostFailures"],
        );

        assert.eq(result.nSuccessfulOperations, 1, "Query should succeed");
        assert.gt(
            result.metricDeltas.extensionSuccesses,
            0,
            `extensionSuccesses should increase after successful extension work.`,
        );
        assert.eq(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should remain stable after successful extension work.`,
        );
        // Note, hostSuccesses increases anytime the extension calls back into the host. This can be
        // an indeterminate number of times in the successful case.
        assert.gte(
            result.metricDeltas.hostSuccesses,
            0,
            `hostSuccesses should not decrease after successful host work.`,
        );
        assert.eq(result.metricDeltas.hostFailures, 0, `hostFailures should remain stable after successful host work.`);
    });

    it("should NOT increase extensionSuccesses for queries without extension stages", function () {
        const result = observeExtensionMetricsChange(
            [
                {
                    operation: () => this.coll.aggregate([{$match: {value: {$gte: 0}}}]).toArray(),
                    expectSuccess: true,
                },
            ],
            () => getTotalMetrics(db),
            ["extensionSuccesses", "extensionFailures", "hostSuccesses", "hostFailures"],
        );

        assert.eq(result.nSuccessfulOperations, 1, "Query should succeed");
        assert.eq(
            result.metricDeltas.extensionSuccesses,
            0,
            `extensionSuccesses should remain stable for query not using extension stages.`,
        );
        assert.eq(
            result.metricDeltas.extensionFailures,
            0,
            `extensionFailures should remain stable for query not using extension stages.`,
        );
        assert.eq(
            result.metricDeltas.hostSuccesses,
            0,
            `hostSuccesses should remain stable for query not using extension stages.`,
        );
        assert.eq(
            result.metricDeltas.hostFailures,
            0,
            `hostFailures should remain stable for query not using extension stages.`,
        );
    });
});
