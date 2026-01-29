/**
 * Tests that serverStatus tracks and reports time spent inside extension getNext() code.
 *
 * This test uses the $sleep extension stage which sleeps for a configurable duration
 * in each getNext() call
 *
 * In sharded environments, the $sleep stage may run on mongos or on shards depending on
 * pipeline position. The test aggregates metrics from all nodes to verify correct tracking.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// The 'serverStatus' command is unreliable in test suites with multiple mongos processes given that
// each node has its own metrics. The assertions here would not hold up if run against multiple
// mongos.
TestData.pinToSingleMongos = true;

/**
 * Helper to get the extension getNext time metric from a single node's serverStatus.
 */
function getMetricFromNode(conn) {
    const serverStatus = conn.getDB("admin").runCommand({serverStatus: 1});
    assert.commandWorked(serverStatus);
    return serverStatus.metrics.extension.totalAggStageExecMicros;
}

/**
 * Gets the sum of extension getNext time metrics from all relevant nodes.
 * In a sharded cluster, this includes mongos and all shard primaries.
 * In a standalone/replica set, this is just the current node.
 */
function getTotalExtensionGetNextTimeMicros(testDb) {
    let total = 0;

    if (FixtureHelpers.isMongos(testDb)) {
        // Get metric from mongos (stages that run on mongos, e.g., after $sort).
        total += getMetricFromNode(testDb.getMongo());

        // Get metrics from all shard primaries (stages that run on shards).
        const primaries = FixtureHelpers.getPrimaries(testDb);
        for (const primary of primaries) {
            total += getMetricFromNode(primary);
        }
    } else {
        // Standalone or replica set - just check the current node.
        total = getMetricFromNode(testDb.getMongo());
    }

    return total;
}

describe("Extension getNext time serverStatus metric", function () {
    before(function () {
        this.coll = db[jsTestName()];
        this.coll.drop();

        // Insert test documents.
        this.numDocs = 20;
        const docs = [];
        for (let i = 0; i < this.numDocs; i++) {
            docs.push({_id: i, value: i * 10});
        }
        assert.commandWorked(this.coll.insertMany(docs));

        jsTest.log.info(`Test setup complete: inserted ${this.numDocs} documents`);
        jsTest.log.info(`Running on mongos: ${FixtureHelpers.isMongos(db)}`);
    });

    after(function () {
        this.coll.drop();
    });

    it("should have a non-negative initial value", function () {
        const initialValue = getTotalExtensionGetNextTimeMicros(db);
        assert.gte(initialValue, 0, `Metric should be non-negative, got: ${initialValue}`);
    });

    it("should increase when $sleep runs on shards (early in pipeline)", function () {
        const beforeValue = getTotalExtensionGetNextTimeMicros(db);

        // Run a query with $sleep early in pipeline (runs on shards in sharded env).
        const sleepMs = 5;
        const result = this.coll.aggregate([{$sleep: {millis: sleepMs}}]).toArray();
        assert.eq(result.length, this.numDocs);

        const afterValue = getTotalExtensionGetNextTimeMicros(db);

        // The metric should have increased.
        assert.gt(afterValue, beforeValue, "Metric should increase after extension work.");

        // Verify the increase is reasonable (at least 50% of expected sleep time to account for
        // parallelism in sharded clusters where work is split across shards).
        const expectedMinMicros = this.numDocs * sleepMs * 1000 * 0.5;
        const increase = afterValue - beforeValue;
        assert.gte(increase, expectedMinMicros);
    });

    it("should increase when $sleep runs on mongos (after $sort)", function () {
        const beforeValue = getTotalExtensionGetNextTimeMicros(db);

        // Run a query with $sleep after $sort.
        // In sharded clusters, $sort causes merging on mongos, so $sleep runs on mongos.
        const sleepMs = 5;
        const result = this.coll.aggregate([{$sort: {value: 1}}, {$sleep: {millis: sleepMs}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterValue = getTotalExtensionGetNextTimeMicros(db);

        // The metric should have increased.
        assert.gt(afterValue, beforeValue, "Metric should increase after extension work.");

        // For stages running on mongos (after merge), all docs flow through a single node.
        const expectedMinMicros = this.numDocs * sleepMs * 1000 * 0.8;
        const increase = afterValue - beforeValue;
        assert.gte(increase, expectedMinMicros);
    });

    it("should accumulate across multiple queries", function () {
        const startValue = getTotalExtensionGetNextTimeMicros(db);

        // Run multiple queries with extension stages.
        const numQueries = 3;
        const sleepMs = 2;
        for (let i = 0; i < numQueries; i++) {
            const result = this.coll.aggregate([{$sleep: {millis: sleepMs}}]).toArray();
            assert.eq(result.length, this.numDocs, `Query ${i + 1} should return all documents`);
        }

        const endValue = getTotalExtensionGetNextTimeMicros(db);

        // The accumulated time should have increased.
        const increase = endValue - startValue;
        // Use lower bound for sharded (parallelism) environments.
        const expectedMinMicros = numQueries * this.numDocs * sleepMs * 1000 * 0.3;
        assert.gte(increase, expectedMinMicros);
    });

    it("should NOT increase for queries without extension stages", function () {
        const beforeValue = getTotalExtensionGetNextTimeMicros(db);

        // Run a query WITHOUT any extension stage.
        const result = this.coll.aggregate([{$match: {value: {$gte: 0}}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterValue = getTotalExtensionGetNextTimeMicros(db);

        // The metric should not have changed.
        assert.eq(afterValue, beforeValue, "Metric should not change without extension stages.");
    });

    it("should have minimal overhead when $sleep has no sleep time", function () {
        const beforeValue = getTotalExtensionGetNextTimeMicros(db);

        // Run a query with $sleep but no sleep time (acts as pass-through).
        const result = this.coll.aggregate([{$sleep: {}}]).toArray();
        assert.eq(result.length, this.numDocs, "Query should return all documents");

        const afterValue = getTotalExtensionGetNextTimeMicros(db);
        const increase = afterValue - beforeValue;

        // The increase should be small (just overhead, not sleep time).
        // Allow more overhead in sharded environments due to multiple nodes.
        const maxExpectedMicros = FixtureHelpers.isMongos(db) ? 50000 : 10000;
        assert.lt(increase, maxExpectedMicros);
    });
});
