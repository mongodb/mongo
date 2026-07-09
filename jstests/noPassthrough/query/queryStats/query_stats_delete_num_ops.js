/**
 * This test confirms that the nDeleteOps metric (tracking the number of delete operation entries
 * in a single delete command) is correctly recorded in query stats for each delete op entry.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getLatestQueryStatsEntry,
    getQueryStatsDeleteCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();

function validateDeleteEntry(entry, sum, max, min) {
    assert.eq(entry.key.queryShape.command, "delete");
    assert(
        entry.metrics.writes.hasOwnProperty("nDeleteOps"),
        "Expected nDeleteOps in writes metrics",
        {
            entry,
        },
    );
    assert.eq(entry.metrics.writes.nDeleteOps.sum, sum, `Expected nDeleteOps.sum to be ${sum}`, {
        entry,
    });
    assert.eq(entry.metrics.writes.nDeleteOps.max, max, `Expected nDeleteOps.max to be ${max}`, {
        entry,
    });
    assert.eq(entry.metrics.writes.nDeleteOps.min, min, `Expected nDeleteOps.min to be ${min}`, {
        entry,
    });
}

/**
 * Run test suite against a specific topology.
 *
 * @param {string} topologyName - Name of the topology (e.g. "Standalone", "Sharded")
 * @param {Function} setupFn - Returns {fixture, testDB}
 * @param {Function} teardownFn - Takes {fixture} and cleans up
 * @param {Function} validateFn - Optional function to validate cluster setup. Called after data is inserted, takes {testDB, coll}
 */
function runDeleteCmdMetricsTests(topologyName, setupFn, teardownFn, validateFn = null) {
    describe(`query stats nDeleteOps metric (${topologyName})`, function () {
        let fixture;
        let testDB;
        let coll;

        function resetCollection() {
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(
                coll.insertMany([
                    {_id: -4, v: 1},
                    {_id: -3, v: 2},
                    {_id: -2, v: 3},
                    {_id: -1, v: 4},
                    {_id: 1, v: 5},
                    {_id: 2, v: 6},
                    {_id: 3, v: 7},
                    {_id: 4, v: 8},
                ]),
            );
        }

        before(function () {
            const setupRes = setupFn();
            fixture = setupRes.fixture;
            testDB = setupRes.testDB;
            coll = testDB[collName];
            resetCollection();
            if (validateFn) {
                validateFn({testDB, coll});
            }
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

        it("test single delete entry", function () {
            const deleteCmd = {
                delete: collName,
                deletes: [{q: {v: 1}, limit: 1}],
                comment: "single delete op entry",
            };

            assert.commandWorked(testDB.runCommand(deleteCmd));

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            validateDeleteEntry(entry, 1, 1, 1);
        });

        it("test different query shapes", function () {
            const deleteCmd = {
                delete: collName,
                deletes: [
                    {q: {v: 1, x: 1}, limit: 1},
                    {q: {v: 2, x: 1, z: 2}, limit: 1},
                    {q: {v: 3, x: 1, z: 2, a: 3}, limit: 1},
                    {q: {v: 4, x: 1, z: 2, a: 3, b: 4}, limit: 1},
                ],
                comment: "four delete op entries",
            };

            assert.commandWorked(testDB.runCommand(deleteCmd));

            // Each delete op entry should generate its own query stats entry with nDeleteOps=4
            // since it has a distinct shape.
            const entries = getQueryStatsDeleteCmd(testDB.getMongo(), {collName: coll.getName()});

            assert.eq(entries.length, 4, "Expected 4 query stats entries", {entries});

            for (const entry of entries) {
                // Each entry should report that there were 4 delete ops in the command.
                validateDeleteEntry(entry, 4, 4, 4);
            }
        });

        it("test same query shape", function () {
            const deleteCmd = {
                delete: collName,
                deletes: [
                    {q: {v: 1}, limit: 1},
                    {q: {v: 2}, limit: 1},
                    {q: {v: 3}, limit: 1},
                    {q: {v: 4}, limit: 1},
                ],
                comment: "four delete op entries with the same shape",
            };

            assert.commandWorked(testDB.runCommand(deleteCmd));

            // Each delete op entry adds to the same entry since shapes are identical.
            const entries = getQueryStatsDeleteCmd(testDB.getMongo(), {collName: coll.getName()});

            assert.eq(entries.length, 1, "Expected one query stats entry", {entries});

            for (const entry of entries) {
                // Each of the 4 per-op recordings reports nDeleteOps=4, so sum = 16.
                validateDeleteEntry(entry, 16, 4, 4);
            }
        });

        it("should aggregate nDeleteOps across multiple executions", function () {
            // Run a 2-entry delete command twice.
            const deleteCmd = {
                delete: collName,
                deletes: [
                    {q: {v: 1}, limit: 1},
                    {q: {v: 2, a: 1}, limit: 1},
                ],
                comment: "two delete op entries - aggregation test",
            };

            assert.commandWorked(testDB.runCommand(deleteCmd));

            deleteCmd.deletes = [
                {q: {v: 3}, limit: 1},
                {q: {v: 4, a: 1}, limit: 1},
            ];
            assert.commandWorked(testDB.runCommand(deleteCmd));

            const entries = getQueryStatsDeleteCmd(testDB.getMongo(), {collName: coll.getName()});

            // We expect 2 query stats entries (one per unique delete op shape).
            assert.eq(entries.length, 2, "Expected 2 query stats entries", {entries});

            for (const entry of entries) {
                // Each entry was executed twice, each time with nDeleteOps=2. Sum should be 4 (2 executions * 2 ops)
                validateDeleteEntry(entry, 4, 2, 2);
            }
        });
    });
}

runDeleteCmdMetricsTests(
    "Standalone",
    () => {
        const conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryStatsSampleRate: 1,
                internalQueryStatsWriteCmdSampleRate: 1,
            },
        });
        const testDB = conn.getDB("test");
        testDB[collName].drop();
        return {fixture: conn, testDB};
    },
    (fixture) => MongoRunner.stopMongod(fixture),
);

runDeleteCmdMetricsTests(
    "Sharded",
    () => {
        const st = new ShardingTest({
            shards: 2,
            mongosOptions: {
                setParameter: {
                    internalQueryStatsRateLimit: -1,
                    internalQueryStatsSampleRate: 1,
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
        });
        const testDB = st.s.getDB("test");
        st.shardColl(testDB[collName], {_id: 1}, {_id: 1}, {_id: 1});
        return {fixture: st, testDB};
    },
    (fixture) => fixture.stop(),
    ({testDB, coll}) => {
        const stats = assert.commandWorked(testDB.runCommand({collStats: coll.getName()}));
        assert.gte(Object.keys(stats.shards).length, 2, "Expected data on at least 2 shards", {
            shards: stats.shards,
        });
    },
);
