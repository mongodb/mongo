/**
 * This test confirms that the nUpdateOps metric (tracking the number of update operation entries
 * in a single update command) is correctly recorded in query stats for each update op entry.
 *
 * @tags: [featureFlagQueryStatsUpdateCommand]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getLatestQueryStatsEntry,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();
/**
 * Run test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 */
function runUpdateCmdMetricsTests(topologyName, setupFn, teardownFn) {
    describe(`query stats nUpdateOps metric (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        function resetCollection() {
            coll.drop();
            assert.commandWorked(coll.insert([{v: 1}, {v: 2}, {v: 3}, {v: 4}, {v: 5}, {v: 6}, {v: 7}, {v: 8}]));
        }

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];
            resetCollection();
        });

        after(function () {
            if (fixture) {
                teardownFn(fixture);
            }
        });

        beforeEach(function () {
            resetCollection();
            resetQueryStatsStore(testDB, "1MB");
        });

        it("test single update entry", function () {
            const updateCmd = {
                update: collName,
                updates: [{q: {v: 1}, u: {$set: {updated: true}}, multi: false}],
                comment: "single update op entry",
            };

            assert.commandWorked(testDB.runCommand(updateCmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "update");

            // Verify that the nUpdateOps metric is present and equals 1.
            assert(
                entry.metrics.writes.hasOwnProperty("nUpdateOps"),
                "Expected nUpdateOps in writes metrics: " + tojson(entry),
            );
            assert.eq(
                entry.metrics.writes.nUpdateOps.sum,
                1,
                "Expected nUpdateOps.sum to be 1 for single update entry",
            );
        });

        it("test different query shapes", function () {
            const updateCmd = {
                update: collName,
                updates: [
                    {q: {v: 1, x: 1}, u: {$set: {updated: true}}, multi: false},
                    {q: {v: 2, x: 1, z: 2}, u: {$set: {updated: true}}, multi: false},
                    {q: {v: 3, x: 1, z: 2, a: 3}, u: {$set: {updated: true}}, multi: false},
                    {q: {v: 4, x: 1, z: 2, a: 3, b: 4}, u: {$set: {updated: true}}, multi: false},
                ],
                comment: "four update op entries",
            };

            assert.commandWorked(testDB.runCommand(updateCmd));

            // Each update op entry should generate its own query stats entry with nUpdateOps=4 since it has a distinct shape.
            const entries = getQueryStatsUpdateCmd(testDB.getMongo(), {collName: coll.getName()});

            assert.eq(entries.length, 4, "Expected 4 query stats entries: " + tojson(entries));

            for (const entry of entries) {
                assert.eq(entry.key.queryShape.command, "update");
                assert(
                    entry.metrics.writes.hasOwnProperty("nUpdateOps"),
                    "Expected nUpdateOps in writes metrics: " + tojson(entry),
                );
                // Each entry should report that there were 4 update ops in the command.
                assert.eq(entry.metrics.writes.nUpdateOps.sum, 4, "Expected nUpdateOps.sum to be 4: " + tojson(entry));
                assert.eq(entry.metrics.writes.nUpdateOps.max, 4, "Expected nUpdateOps.min to be 4: " + tojson(entry));
                assert.eq(entry.metrics.writes.nUpdateOps.min, 4, "Expected nUpdateOps.max to be 4: " + tojson(entry));
            }
        });

        it("test same query shape", function () {
            const updateCmd = {
                update: collName,
                updates: [
                    {q: {v: 1}, u: {$set: {updated: true}}, multi: false},
                    {q: {v: 2}, u: {$set: {updated: true}}, multi: false},
                    {q: {v: 3}, u: {$set: {updated: true}}, multi: false},
                    {q: {v: 4}, u: {$set: {updated: true}}, multi: false},
                ],
                comment: "four update op entries with the same shape",
            };

            assert.commandWorked(testDB.runCommand(updateCmd));

            // Each update op entry should add to the same entry.
            const entries = getQueryStatsUpdateCmd(testDB.getMongo(), {collName: coll.getName()});

            assert.eq(entries.length, 1, "Expected one query stats entry: " + tojson(entries));

            for (const entry of entries) {
                assert.eq(entry.key.queryShape.command, "update");
                assert(
                    entry.metrics.writes.hasOwnProperty("nUpdateOps"),
                    "Expected nUpdateOps in writes metrics: " + tojson(entry),
                );
                // Each update should upate the nUpdateOps value.
                assert.eq(
                    entry.metrics.writes.nUpdateOps.sum,
                    16,
                    "Expected nUpdateOps.sum to be 16: " + tojson(entry),
                );
                // Each recording should have updated the value by 4.
                assert.eq(entry.metrics.writes.nUpdateOps.max, 4, "Expected nUpdateOps.min to be 4: " + tojson(entry));
                assert.eq(entry.metrics.writes.nUpdateOps.min, 4, "Expected nUpdateOps.max to be 4: " + tojson(entry));
            }
        });

        it("should aggregate nUpdateOps across multiple executions", function () {
            // Run a 2-entry update command twice.
            const updateCmd = {
                update: collName,
                updates: [
                    {q: {v: 1}, u: {$set: {count: 1}}, multi: false},
                    {q: {v: 2, a: 1}, u: {$set: {count: 1}}, multi: false},
                ],
                comment: "two update op entries - aggregation test",
            };

            // Execute twice.
            assert.commandWorked(testDB.runCommand(updateCmd));
            assert.commandWorked(testDB.runCommand(updateCmd));

            const entries = getQueryStatsUpdateCmd(testDB.getMongo(), {collName: coll.getName()});

            // We expect 2 query stats entries (one per unique update op shape).
            assert.eq(entries.length, 2, "Expected 2 query stats entries: " + tojson(entries));

            for (const entry of entries) {
                assert.eq(entry.key.queryShape.command, "update");
                assert(
                    entry.metrics.writes.hasOwnProperty("nUpdateOps"),
                    "Expected nUpdateOps in writes metrics: " + tojson(entry),
                );
                // Each entry was executed twice, each time with nUpdateOps=2.
                assert.eq(
                    entry.metrics.writes.nUpdateOps.sum,
                    4,
                    "Expected nUpdateOps.sum to be 4 (2 executions * 2 ops): " + tojson(entry),
                );
                assert.eq(entry.metrics.writes.nUpdateOps.max, 2, "Expected nUpdateOps.max to be 2: " + tojson(entry));
                assert.eq(entry.metrics.writes.nUpdateOps.min, 2, "Expected nUpdateOps.min to be 2: " + tojson(entry));
            }
        });
    });
}

runUpdateCmdMetricsTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({setParameter: {internalQueryStatsRateLimit: -1}});
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

runUpdateCmdMetricsTests(
    "Sharded",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
        });
        const testDB = st.s.getDB("test");
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1});
        return {fixture: st, testDB};
    },
    (st) => st.stop(),
);
