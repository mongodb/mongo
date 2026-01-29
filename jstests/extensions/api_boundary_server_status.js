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

// The 'serverStatus' command is unreliable in test suites with multiple mongos processes given that
// each node has its own metrics. The assertions here would not hold up if run against multiple
// mongos.
TestData.pinToSingleMongos = true;

/**
 * Helper to get an extension server status metric.
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

function getTotalExtensionSuccesses(testDb) {
    return getTotalExtensionMetrics(testDb, "extensionSuccesses");
}

function getTotalExtensionFailures(testDb) {
    return getTotalExtensionMetrics(testDb, "extensionFailures");
}

function getTotalHostSuccesses(testDb) {
    return getTotalExtensionMetrics(testDb, "hostSuccesses");
}

function getTotalHostFailures(testDb) {
    return getTotalExtensionMetrics(testDb, "hostFailures");
}

function getTotalMetrics(testDB) {
    return {
        "extensionSuccesses": getTotalExtensionSuccesses(testDB),
        "extensionFailures": getTotalExtensionFailures(testDB),
        "hostSuccesses": getTotalHostSuccesses(testDB),
        "hostFailures": getTotalHostFailures(testDB),
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
    // TODO SERVER-118486: Leverage observeExtensionMetricsChange() helper in this jstest, modifying the helper as needed.
    it("should have non-negative initial values", function () {
        const initialMetrics = getTotalMetrics(db);
        assert.gte(initialMetrics["extensionSuccesses"], 0, `extensionSuccesses should be non-negative.`);
        assert.gte(initialMetrics["extensionFailures"], 0, `extensionFailures should be non-negative.`);
        assert.gte(initialMetrics["hostSuccesses"], 0, `hostSuccesses should be non-negative.`);
        assert.gte(initialMetrics["hostFailures"], 0, `hostFailures should be non-negative.`);
    });

    it("should increase extensionSuccesses when $testFoo runs successfully", function () {
        const beforeMetrics = getTotalMetrics(db);

        // Run a query with $testFoo early in pipeline (runs on shards in sharded env).
        const result = this.coll.aggregate([{$testFoo: {}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterMetrics = getTotalMetrics(db);
        assert.gt(
            afterMetrics["extensionSuccesses"],
            beforeMetrics["extensionSuccesses"],
            "extensionSuccesses should increase after successful extension work.",
        );
        assert.eq(
            afterMetrics["extensionFailures"],
            beforeMetrics["extensionFailures"],
            `extensionFailures should remain stable after successful extension work.`,
        );
        // Note, hostSuccesses increases anytime the extension calls back into the host. This can be
        // an indeterminate number of times in the successful case.
        assert.gte(
            afterMetrics["hostSuccesses"],
            beforeMetrics["hostSuccesses"],
            `hostSuccesses should increase after successful host work.`,
        );
        assert.eq(
            afterMetrics["hostFailures"],
            beforeMetrics["hostFailures"],
            `hostFailures should remain stable after successful host work.`,
        );
    });

    it("should increase extensionSuccesses when $testFoo runs on mongos (after $sort)", function () {
        const beforeMetrics = getTotalMetrics(db);

        // Run a query with $testFoo after $sort.
        // In sharded clusters, $sort causes merging on mongos, so $testFoo runs on mongos.
        const result = this.coll.aggregate([{$sort: {value: 1}}, {$testFoo: {}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterMetrics = getTotalMetrics(db);
        assert.gt(
            afterMetrics["extensionSuccesses"],
            beforeMetrics["extensionSuccesses"],
            "extensionSuccesses should increase after successful extension work.",
        );
        assert.eq(
            afterMetrics["extensionFailures"],
            beforeMetrics["extensionFailures"],
            `extensionFailures should remain stable after successful extension work.`,
        );
        assert.gt(
            afterMetrics["hostSuccesses"],
            beforeMetrics["hostSuccesses"],
            `hostSuccesses should increase after successful host work.`,
        );
        assert.eq(
            afterMetrics["hostFailures"],
            beforeMetrics["hostFailures"],
            `hostFailures should remain stable after successful host work.`,
        );
    });

    it("should accumulate extensionSuccesses across multiple queries", function () {
        const numQueries = 3;
        for (let i = 0; i < numQueries; i++) {
            const beforeMetrics = getTotalMetrics(db);

            const result = this.coll.aggregate([{$testFoo: {}}]).toArray();
            assert.eq(result.length, this.numDocs, `Query ${i + 1} should return all documents`);
            const afterMetrics = getTotalMetrics(db);
            assert.gt(
                afterMetrics["extensionSuccesses"],
                beforeMetrics["extensionSuccesses"],
                "extensionSuccesses should accumulate across multiple queries.",
            );
            assert.eq(
                afterMetrics["extensionFailures"],
                beforeMetrics["extensionFailures"],
                `extensionFailures should remain stable across multiple successful queries`,
            );
            // Note, hostSuccesses increases anytime the extension calls back into the host. This
            // can be an indeterminate number of times in the succesful case.
            assert.gte(
                afterMetrics["hostSuccesses"],
                beforeMetrics["hostSuccesses"],
                `hostSuccesses should increase across multiple queries.`,
            );
            assert.eq(
                afterMetrics["hostFailures"],
                beforeMetrics["hostFailures"],
                `hostFailures should remain stable across multiple successful queries.`,
            );
        }
    });

    it("extension and host failures should both increase when $assert triggers a uassert in parse phase", function () {
        const beforeMetrics = getTotalMetrics(db);

        // Run a query with $assert that will trigger a uassert failure at parse time.
        const assertResult = db.runCommand({
            aggregate: this.coll.getName(),
            pipeline: [
                {
                    $assert: {errmsg: "test uassert failure", code: 11569609, assertionType: "uassert"},
                },
            ],
            cursor: {},
        });

        assert.commandFailedWithCode(assertResult, 11569609, "Assert should fail with expected code");

        const afterMetrics = getTotalMetrics(db);
        assert.gte(
            afterMetrics["extensionSuccesses"],
            beforeMetrics["extensionSuccesses"],
            `extensionSuccesses should not decrease after extension failure at parse time.`,
        );
        assert.gt(
            afterMetrics["extensionFailures"],
            beforeMetrics["extensionFailures"],
            `extensionFailures should increase after extension failure at parse time.`,
        );
        assert.gte(
            afterMetrics["hostSuccesses"],
            beforeMetrics["hostSuccesses"],
            `hostSuccesses should not decrease after extension failure at parse time.`,
        );
        assert.gt(
            afterMetrics["hostFailures"],
            beforeMetrics["hostFailures"],
            `hostFailures should increase due to host triggered uassert at parse time.`,
        );
    });

    it("should increase both extensionSuccess and extensionFailures when $assert triggers a uassert in ast phase", function () {
        const beforeMetrics = getTotalMetrics(db);

        // Run a query with $assert that will trigger a uassert failure at ast creation time.
        const assertResult = db.runCommand({
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
        });

        assert.commandFailedWithCode(assertResult, 11569610, "Assert should fail with expected code");

        const afterMetrics = getTotalMetrics(db);
        assert.gt(
            afterMetrics["extensionSuccesses"],
            beforeMetrics["extensionSuccesses"],
            `extensionSuccesses should increase after extension failure in ast phase.`,
        );
        assert.gt(
            afterMetrics["extensionFailures"],
            beforeMetrics["extensionFailures"],
            `extensionFailures should increase after extension failure in ast phase.`,
        );
        assert.gte(
            afterMetrics["hostSuccesses"],
            beforeMetrics["hostSuccesses"],
            `hostSuccesses should not decrease after extension failure in ast phase.`,
        );
        assert.gt(
            afterMetrics["hostFailures"],
            beforeMetrics["hostFailures"],
            `hostFailures should increase due to host triggered uassert in ast phase.`,
        );
    });

    it("should NOT increase extensionFailures for successful queries", function () {
        const beforeMetrics = getTotalMetrics(db);
        // extension failures should not increase

        const result = this.coll.aggregate([{$testFoo: {}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterMetrics = getTotalMetrics(db);
        assert.gt(
            afterMetrics["extensionSuccesses"],
            beforeMetrics["extensionSuccesses"],
            `extensionSuccesses should increase after successful extension work.`,
        );
        assert.eq(
            afterMetrics["extensionFailures"],
            beforeMetrics["extensionFailures"],
            `extensionFailures should remain stable after successful extension work.`,
        );
        // Note, hostSuccesses increases anytime the extension calls back into the host. This can be
        // an indeterminate number of times in the succesful case.
        assert.gte(
            afterMetrics["hostSuccesses"],
            beforeMetrics["hostSuccesses"],
            `hostSuccesses should increase after successful host work.`,
        );
        assert.eq(
            afterMetrics["hostFailures"],
            beforeMetrics["hostFailures"],
            `hostFailures should remain stable after successful host work.`,
        );
    });

    it("should NOT increase extensionSuccesses for queries without extension stages", function () {
        const beforeMetrics = getTotalMetrics(db);

        const result = this.coll.aggregate([{$match: {value: {$gte: 0}}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterMetrics = getTotalMetrics(db);
        assert.eq(
            afterMetrics["extensionSuccesses"],
            beforeMetrics["extensionSuccesses"],
            `extensionSuccesses should remain stable for query not using extension stages.`,
        );
        assert.eq(
            afterMetrics["extensionFailures"],
            beforeMetrics["extensionFailures"],
            `extensionFailures should remain stable for query not using extension stages.`,
        );
        assert.eq(
            afterMetrics["hostSuccesses"],
            beforeMetrics["hostSuccesses"],
            `hostSuccesses should remain stable for query not using extension stages.`,
        );
        assert.eq(
            afterMetrics["hostFailures"],
            beforeMetrics["hostFailures"],
            `hostFailures should remain stable for query not using extension stages.`,
        );
    });
});
