/**
 * Tests that $queryStats records errors per query shape, including a counter of errored executions
 * and an LRU-bounded errors array of {code, codeName, count, latestSeenTimestamp}.
 *
 * Right now, only non-cursor read errors are supported for query stats error collection, on both
 * mongod and mongos. Errors should be recorded for find/aggregate that error during the first batch
 * and count/distinct, which are inherently non-cursor.
 *
 * @tags: [featureFlagQueryStatsErrors, featureFlagQueryStatsCountDistinct, requires_sharding]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getLatestQueryStatsEntry,
    getQueryStatsServerParameters,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kPlanExecutorFailCode = 4382101;
const kPlanExecutorFailCodeName = `Location${kPlanExecutorFailCode}`;

function queryStatsOptions() {
    const options = getQueryStatsServerParameters();
    options.setParameter.featureFlagQueryStatsErrors = true;
    options.setParameter.featureFlagQueryStatsCountDistinct = true;
    return options;
}

function errorCountByName(metrics, codeName) {
    const match = metrics?.errors.find((e) => e.codeName === codeName);
    return match?.count ?? 0;
}

// Asserts the invariants for a shape that has only ever errored.
function assertOnlyErroredEntry(
    metrics,
    {execCountErrored, codeName, codeCount},
    errorDescription,
) {
    // Errored executions are counted only by 'execCountErrored', so a shape that has only ever
    // errored leaves 'execCount' at 0.
    assert.eq(0, metrics.execCount, `${errorDescription}: execCount`, {metrics});
    assert.eq(execCountErrored, metrics.execCountErrored, `${errorDescription}: execCountErrored`, {
        metrics,
    });
    assert(metrics.hasOwnProperty("errors"), `${errorDescription}: missing errors`, {metrics});
    assert.eq(1, metrics.errors.length, `${errorDescription}: expected one error bucket`, {
        metrics,
    });
    assert.eq(codeCount, errorCountByName(metrics, codeName), `${errorDescription}: error count`, {
        metrics,
    });
    // An errored execution must not contribute timing samples.
    assert.eq(
        0,
        metrics.totalExecMicros.sum,
        `${errorDescription}: standard query stats metrics not recorded`,
        {
            metrics,
        },
    );
}

/**
 * On mongod, the following is asserted:
 *  - A successful execution leaves 'execCountErrored' at 0 and omits the 'errors' array,
 *  - An execution error bumps 'execCountErrored', records the code in 'errors', and does
 *    not fold partial timing into the aggregates,
 *  - errored executions are counted only by 'execCountErrored', leaving 'execCount' for
 *    executions that recorded standard metrics,
 *  - Repeated errors on the same shape accumulate on the same entry,
 * for both execution-time throws (using planExecutorAlwaysFails) and deadline kills (hang failpoint
 * and short maxTimeMS). Aggregate and distinct lack a post-registration hang failpoint, so only
 * execution-time throws are covered.
 */
describe("query stats errored", function () {
    let conn, testDB, coll;

    before(function () {
        conn = MongoRunner.runMongod(queryStatsOptions());
        testDB = conn.getDB("test");
        coll = testDB[jsTestName()];
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
                {_id: 2, a: 3},
            ]),
        );
    });

    beforeEach(function () {
        resetQueryStatsStore(conn, "1MB");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function getLatestQueryStatsMetrics() {
        const entry = getLatestQueryStatsEntry(conn, {collName: coll.getName()});
        assert(entry.hasOwnProperty("metrics"), "missing metrics", {entry});
        return entry.metrics;
    }

    it("does not record errored fields on a successful execution", function () {
        assert.eq(2, coll.find({a: {$gte: 2}}).itcount());
        const metrics = getLatestQueryStatsMetrics();
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assert.eq(0, metrics.execCountErrored, "execCountErrored", {metrics});
        assert(!metrics.hasOwnProperty("errors"), "unexpected errors array", {metrics});
    });

    it("keeps successful timing when the same shape later errors", function () {
        const filter = {mixed: {$gte: 0}};
        assert.commandWorked(testDB.runCommand({find: coll.getName(), filter}));
        const timingAfterSuccess = getLatestQueryStatsMetrics().totalExecMicros.sum;
        assert.gt(timingAfterSuccess, 0, "success should record timing");

        // The same shape now errors, recording on the same entry.
        const fp = configureFailPoint(testDB, "planExecutorAlwaysFails");
        try {
            assert.commandFailedWithCode(
                testDB.runCommand({find: coll.getName(), filter}),
                kPlanExecutorFailCode,
            );
        } finally {
            fp.off();
        }

        const metrics = getLatestQueryStatsMetrics();
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assert.eq(1, metrics.execCountErrored, "execCountErrored", {metrics});
        assert.eq(1, errorCountByName(metrics, kPlanExecutorFailCodeName), "error count", {
            metrics,
        });
        assert.eq(timingAfterSuccess, metrics.totalExecMicros.sum, "timing changed", {metrics});
    });

    it("records execution-time errors for each non-cursor read command", function () {
        // 'planExecutorAlwaysFails' makes the plan executor throw during getNext, so the key is
        // still live on OpDebug when the operation completes with an error, and the shape is stable
        // across runs.
        const execErrorCases = [
            {name: "find", cmd: {find: coll.getName(), filter: {findErr: {$gte: 0}}}},
            {
                name: "aggregate",
                cmd: {
                    aggregate: coll.getName(),
                    pipeline: [{$match: {aggErr: {$gte: 0}}}],
                    cursor: {},
                },
            },
            {name: "count", cmd: {count: coll.getName(), query: {countErr: {$gte: 0}}}},
            {name: "distinct", cmd: {distinct: coll.getName(), key: "distinctErr"}},
        ];

        const fp = configureFailPoint(testDB, "planExecutorAlwaysFails");
        try {
            for (const {name, cmd} of execErrorCases) {
                assert.commandFailedWithCode(
                    testDB.runCommand(cmd),
                    kPlanExecutorFailCode,
                    `expected ${name} to error`,
                );
                assertOnlyErroredEntry(
                    getLatestQueryStatsMetrics(),
                    {
                        execCountErrored: 1,
                        codeName: kPlanExecutorFailCodeName,
                        codeCount: 1,
                    },
                    `${name} first error`,
                );

                // Running the same erroring shape again increments both counters.
                assert.commandFailedWithCode(
                    testDB.runCommand(cmd),
                    kPlanExecutorFailCode,
                    `expected ${name} to error again`,
                );
                assertOnlyErroredEntry(
                    getLatestQueryStatsMetrics(),
                    {
                        execCountErrored: 2,
                        codeName: kPlanExecutorFailCodeName,
                        codeCount: 2,
                    },
                    `${name} second error`,
                );
            }
        } finally {
            fp.off();
        }
    });

    it("records deadline kills for find and count", function () {
        // Only commands with a post-registration hang failpoint.
        const deadlineCases = [
            {
                name: "find",
                failPoint: "waitInFindBeforeMakingBatch",
                cmd: {find: coll.getName(), filter: {findTimeout: {$lte: 100}}, maxTimeMS: 500},
            },
            {
                name: "count",
                failPoint: "hangBeforeCollectionCount",
                cmd: {count: coll.getName(), query: {countTimeout: {$lte: 100}}, maxTimeMS: 500},
            },
        ];
        for (const {name, failPoint, cmd} of deadlineCases) {
            const fp = configureFailPoint(testDB, failPoint, {shouldCheckForInterrupt: true});
            try {
                assert.commandFailedWithCode(
                    testDB.runCommand(cmd),
                    ErrorCodes.MaxTimeMSExpired,
                    `${name} should have been killed by its deadline`,
                );
            } finally {
                fp.off();
            }

            assertOnlyErroredEntry(
                getLatestQueryStatsMetrics(),
                {execCountErrored: 1, codeName: "MaxTimeMSExpired", codeCount: 1},
                `${name} deadline kill`,
            );
        }
    });
});

// Runs 'fn' with 'failPoint' enabled on every connection in 'conns'.
function withFailPoint(conns, failPoint, data, fn) {
    const failPoints = conns.map((conn) => configureFailPoint(conn, failPoint, data));
    try {
        fn();
    } finally {
        for (const fp of failPoints) {
            fp.off();
        }
    }
}

// Runs 'fn' with each connection in 'conns' failing 'cmdNames' with 'errorCode'.
// 'failInternalCommands' is required because mongos reaches the shards as an internal client.
function withCommandFailure(conns, cmdNames, errorCode, fn) {
    withFailPoint(
        conns,
        "failCommand",
        {errorCode: errorCode, failCommands: cmdNames, failInternalCommands: true},
        fn,
    );
}

/**
 * On mongos the errors are grouped by where the failure originates, since that decides which code
 * path is exercised:
 *  - shard-level: one of the two targeted shards returns the error and mongos aborts cursor
 *    establishment, unless 'allowPartialResults' lets the operation survive the failing shard (an
 *    option only find and aggregate accept),
 *  - router-level: mongos itself throws while the shards succeed or are never contacted.
 */
describe("query stats errored on mongos", function () {
    let st, testDB, coll;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: queryStatsOptions(),
            rsOptions: queryStatsOptions(),
        });
        testDB = st.getDB("test");
        coll = testDB[jsTestName()];
        coll.drop();
        // Data lives across both shards so reads target more than one shard.
        st.shardColl(coll, {_id: 1}, {_id: 1}, {_id: 1});
        assert.commandWorked(
            coll.insert([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
                {_id: 2, a: 3},
            ]),
        );
    });

    beforeEach(function () {
        resetQueryStatsStore(st.s, "1MB");
    });

    after(function () {
        st.stop();
    });

    function getLatestQueryStatsMetrics() {
        const entry = getLatestQueryStatsEntry(st.s, {collName: coll.getName()});
        assert(entry.hasOwnProperty("metrics"), "missing metrics", {entry});
        return entry.metrics;
    }

    const allShards = () => [st.shard0, st.shard1];
    const oneShard = () => [st.shard0];

    it("does not record errored fields on a successful execution", function () {
        assert.eq(2, coll.find({a: {$gte: 2}}).itcount());
        const metrics = getLatestQueryStatsMetrics();
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assert.eq(0, metrics.execCountErrored, "execCountErrored", {metrics});
        assert(!metrics.hasOwnProperty("errors"), "unexpected errors array", {metrics});
    });

    /**
     * These tests make one of the two targeted shards fail the read. The outcome depends on
     * 'allowPartialResults':
     *  - false (default): the shard's error fails the whole operation, so the key
     *    is still on OpDebug and the completion hook attributes the error to the shape.
     *  - true: mongos ignores the failing shard and returns the surviving shard's results, so the
     *    operation succeeds and nothing errored should be recorded. Only find and aggregate accept
     *    the option. TODO SERVER-131929: Update test when allowPartialResults:true updates stats.
     */
    describe("shard-level failures", function () {
        it("records errors for each non-cursor read command", function () {
            const execErrorCases = [
                {name: "find", cmd: {find: coll.getName(), filter: {findErr: {$gte: 0}}}},
                {
                    name: "aggregate",
                    cmd: {
                        aggregate: coll.getName(),
                        pipeline: [{$match: {aggErr: {$gte: 0}}}],
                        cursor: {},
                    },
                },
                {name: "count", cmd: {count: coll.getName(), query: {countErr: {$gte: 0}}}},
                {name: "distinct", cmd: {distinct: coll.getName(), key: "distinctErr"}},
            ];

            for (const {name, cmd} of execErrorCases) {
                withCommandFailure(oneShard(), [name], ErrorCodes.MaxTimeMSExpired, () => {
                    assert.commandFailedWithCode(
                        testDB.runCommand(cmd),
                        ErrorCodes.MaxTimeMSExpired,
                        `expected ${name} to error`,
                    );
                });

                assertOnlyErroredEntry(
                    getLatestQueryStatsMetrics(),
                    {
                        execCountErrored: 1,
                        codeName: "MaxTimeMSExpired",
                        codeCount: 1,
                    },
                    `shard-level ${name}`,
                );
            }
        });

        it("does not record an error when allowPartialResults is true", function () {
            const partialResultsCases = [
                {
                    name: "find",
                    cmd: {
                        find: coll.getName(),
                        filter: {partialFind: {$gte: 0}},
                        allowPartialResults: true,
                    },
                },
                {
                    name: "aggregate",
                    cmd: {
                        aggregate: coll.getName(),
                        pipeline: [{$match: {partialAgg: {$gte: 0}}}],
                        cursor: {},
                        allowPartialResults: true,
                    },
                },
            ];

            for (const {name, cmd} of partialResultsCases) {
                withCommandFailure(oneShard(), [name], ErrorCodes.MaxTimeMSExpired, () => {
                    const res = assert.commandWorked(testDB.runCommand(cmd));
                    assert(
                        res.cursor.partialResultsReturned,
                        `expected ${name} to return partial results`,
                        {res},
                    );
                });

                const metrics = getLatestQueryStatsMetrics();
                assert.eq(1, metrics.execCount, `${name} execCount`, {metrics});
                assert.eq(0, metrics.execCountErrored, `${name} execCountErrored`, {metrics});
                assert(
                    !metrics.hasOwnProperty("errors"),
                    `${name}: a partial-results success must not be recorded as errored`,
                    {metrics},
                );
            }
        });
    });

    describe("router-level failures", function () {
        it("records command validation failures", function () {
            // Projecting the reserved $sortKey field is rejected by mongos. The shape is registered so
            // this is the earliest point at which the hook should observe a non-cursor error.
            assert.commandFailedWithCode(
                testDB.runCommand({
                    find: coll.getName(),
                    filter: {routerSortKey: {$gte: 0}},
                    projection: {$sortKey: 1},
                }),
                ErrorCodes.BadValue,
                "expected a $sortKey projection to be rejected by the router",
            );

            assertOnlyErroredEntry(
                getLatestQueryStatsMetrics(),
                {execCountErrored: 1, codeName: "BadValue", codeCount: 1},
                "router-level validation: $sortKey projection",
            );
        });

        it("records response validation failures", function () {
            // mongos throws while validating a response the shard returned successfully. This is the
            // last point where the hook should observe a non-cursor error.
            const validationCases = [
                {
                    name: "find",
                    cmd: {find: coll.getName(), filter: {routerValidationFind: {$gte: 0}}},
                },
                {
                    name: "aggregate",
                    cmd: {
                        aggregate: coll.getName(),
                        pipeline: [{$match: {routerValidationAgg: {$gte: 0}}}],
                        cursor: {},
                    },
                },
            ];

            for (const {name, cmd} of validationCases) {
                withFailPoint([st.s], "throwDuringCursorResponseValidation", {}, () => {
                    assert.commandFailedWithCode(
                        testDB.runCommand(cmd),
                        ErrorCodes.FailedToParse,
                        `expected router-level ${name} validation failure`,
                    );
                });

                assertOnlyErroredEntry(
                    getLatestQueryStatsMetrics(),
                    {
                        execCountErrored: 1,
                        codeName: "FailedToParse",
                        codeCount: 1,
                    },
                    `router-level ${name} response validation`,
                );
            }
        });

        it("records deadline kills while the router waits on the shards", function () {
            withFailPoint(
                allShards(),
                "shardWaitInFindBeforeMakingBatch",
                // Shards cannot raise anything so MaxTimeMSExpired is router's own deadline.
                {shouldCheckForInterrupt: false},
                () => {
                    assert.commandFailedWithCode(
                        testDB.runCommand({
                            find: coll.getName(),
                            filter: {routerTimeout: {$lte: 100}},
                            maxTimeMS: 500,
                        }),
                        ErrorCodes.MaxTimeMSExpired,
                        "find should have been killed by the router's deadline",
                    );
                },
            );

            assertOnlyErroredEntry(
                getLatestQueryStatsMetrics(),
                {execCountErrored: 1, codeName: "MaxTimeMSExpired", codeCount: 1},
                "router deadline kill",
            );
        });
    });
});
