/**
 * This test confirms that the nUpdateOps / nDeleteOps metric (tracking the number of update/delete
 * operation entries in a single write command) is correctly recorded in query stats for each op
 * entry.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getLatestQueryStatsEntry,
    getQueryStatsDeleteCmd,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {
    shardedWriteCmdQueryStatsFixture,
    standaloneWriteCmdQueryStatsFixture,
} from "jstests/libs/query/query_stats_write_cmd_utils.js";

const collName = jsTestName();

/**
 * Shared test suite for the nDeleteOps / nUpdateOps "num ops" metric.
 *
 * @param {object} cmdInfo
 *   cmdInfo.command         - "delete" | "update"
 *   cmdInfo.getQueryStatsFn - getQueryStatsDeleteCmd | getQueryStatsUpdateCmd
 * @param {string} topologyName - "Standalone" | "Sharded" (used only in the describe label)
 * @param {Function} setupFn - fixture setup, returns {fixture, testDB} (see *WriteCmdQueryStatsFixture)
 * @param {Function} teardownFn - fixture teardown, takes the fixture
 * @param {Function} [validateFn] - optional post-setup cluster validation, called with {testDB, coll}
 */
function runWriteCmdNumOpsTests(
    {command, getQueryStatsFn},
    topologyName,
    setupFn,
    teardownFn,
    validateFn = null,
) {
    const collName = jsTestName();
    const metricName = command === "delete" ? "nDeleteOps" : "nUpdateOps";
    const opsField = command === "delete" ? "deletes" : "updates";

    // Build a command by wrapping each filter in the op's fixed attributes (delete limit:1 / update $set),
    // so tests vary only the number and shape of the filters.
    const makeCmd = (qList, comment) => ({
        [command]: collName,
        [opsField]: qList.map((q) =>
            command === "delete" ? {q, limit: 1} : {q, u: {$set: {updated: true}}, multi: false},
        ),
        comment,
    });

    function validateEntry(entry, {sum, max, min}) {
        assert.eq(entry.key.queryShape.command, command);
        assert(
            entry.metrics.writes.hasOwnProperty(metricName),
            `Expected ${metricName} in writes metrics`,
            {entry},
        );
        assert.eq(
            entry.metrics.writes[metricName].sum,
            sum,
            `Expected ${metricName}.sum to be ${sum}`,
            {entry},
        );
        assert.eq(
            entry.metrics.writes[metricName].max,
            max,
            `Expected ${metricName}.max to be ${max}`,
            {entry},
        );
        assert.eq(
            entry.metrics.writes[metricName].min,
            min,
            `Expected ${metricName}.min to be ${min}`,
            {entry},
        );
    }

    describe(`query stats ${metricName} metric (${topologyName})`, function () {
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

        it(`test single ${command} entry`, function () {
            assert.commandWorked(
                testDB.runCommand(makeCmd([{v: 1}], `single ${command} op entry`)),
            );

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName: coll.getName()});
            validateEntry(entry, {sum: 1, max: 1, min: 1});
        });

        it("test different query shapes", function () {
            assert.commandWorked(
                testDB.runCommand(
                    makeCmd(
                        [
                            {v: 1, x: 1},
                            {v: 2, x: 1, z: 2},
                            {v: 3, x: 1, z: 2, a: 3},
                            {v: 4, x: 1, z: 2, a: 3, b: 4},
                        ],
                        `four ${command} op entries`,
                    ),
                ),
            );

            // Each op has a distinct shape, so each generates its own query stats entry, and each
            // entry reports that the command carried 4 ops.
            const entries = getQueryStatsFn(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entries.length, 4, "Expected 4 query stats entries", {entries});
            for (const entry of entries) {
                validateEntry(entry, {sum: 4, max: 4, min: 4});
            }
        });

        it("test same query shape", function () {
            assert.commandWorked(
                testDB.runCommand(
                    makeCmd(
                        [{v: 1}, {v: 2}, {v: 3}, {v: 4}],
                        `four ${command} op entries with the same shape`,
                    ),
                ),
            );

            // Each op entry should add to the same entry, since shapes are identical.
            const entries = getQueryStatsFn(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entries.length, 1, "Expected one query stats entry", {entries});
            for (const entry of entries) {
                // Each of the 4 per-op recordings report 4 ops, so sum = 16.
                validateEntry(entry, {sum: 16, max: 4, min: 4});
            }
        });

        it(`should aggregate ${metricName} across multiple executions`, function () {
            // Run a 2-op command twice.
            assert.commandWorked(
                testDB.runCommand(
                    makeCmd([{v: 1}, {v: 2, a: 1}], `two ${command} op entries - aggregation test`),
                ),
            );

            // For delete the first run already removed v:1/v:2, so the second run targets different
            // documents (v:3/v:4) using the same two shapes; for update, the same command is run.
            const secondRunFilters =
                command === "delete" ? [{v: 3}, {v: 4, a: 1}] : [{v: 1}, {v: 2, a: 1}];
            assert.commandWorked(
                testDB.runCommand(
                    makeCmd(secondRunFilters, `two ${command} op entries - aggregation test`),
                ),
            );

            // We expect 2 query stats entries (one per unique op shape)
            const entries = getQueryStatsFn(testDB.getMongo(), {collName: coll.getName()});
            assert.eq(entries.length, 2, "Expected 2 query stats entries", {entries});
            for (const entry of entries) {
                // Each entry was executed twice with Ops=2. Sum should be 4.
                validateEntry(entry, {sum: 4, max: 2, min: 2});
            }
        });
    });
}

// Validates that the sharded fixture distributed data across both shards.
const validateShards = ({testDB, coll}) => {
    const stats = assert.commandWorked(testDB.runCommand({collStats: coll.getName()}));
    assert.gte(Object.keys(stats.shards).length, 2, "Expected data on at least 2 shards", {
        shards: stats.shards,
    });
};

// nDeleteOps tests
const deleteCmdInfo = {command: "delete", getQueryStatsFn: getQueryStatsDeleteCmd};

const deleteStandalone = standaloneWriteCmdQueryStatsFixture({internalQueryStatsSampleRate: 1});
runWriteCmdNumOpsTests(
    deleteCmdInfo,
    "Standalone",
    deleteStandalone.setupFn,
    deleteStandalone.teardownFn,
);

const deleteSharded = shardedWriteCmdQueryStatsFixture(collName, {
    extraSetParameters: {internalQueryStatsSampleRate: 1},
});
runWriteCmdNumOpsTests(
    deleteCmdInfo,
    "Sharded",
    deleteSharded.setupFn,
    deleteSharded.teardownFn,
    validateShards,
);

// nUpdateOps tests
const updateCmdInfo = {command: "update", getQueryStatsFn: getQueryStatsUpdateCmd};

const updateStandalone = standaloneWriteCmdQueryStatsFixture();
runWriteCmdNumOpsTests(
    updateCmdInfo,
    "Standalone",
    updateStandalone.setupFn,
    updateStandalone.teardownFn,
);

const updateSharded = shardedWriteCmdQueryStatsFixture(collName);
runWriteCmdNumOpsTests(
    updateCmdInfo,
    "Sharded",
    updateSharded.setupFn,
    updateSharded.teardownFn,
    validateShards,
);
