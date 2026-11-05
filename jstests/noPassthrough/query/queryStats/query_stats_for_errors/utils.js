/**
 * Shared helpers for $queryStats error-collection tests.
 *
 * When an operation fails while its query stats key is still owned by the operation (i.e. before it
 * is moved onto a cursor), $queryStats records the error against the query shape: an
 * `execCountErrored` counter and an LRU-bounded `errors` array of
 * {code, codeName, count, latestSeenTimestamp}. Errored executions are counted only by
 * `execCountErrored` and never contribute to `execCount` or the timing aggregates.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {it} from "jstests/libs/mochalite.js";
import {
    getLatestQueryStatsEntry,
    getQueryStatsServerParameters,
} from "jstests/libs/query/query_stats_utils.js";

export const kPlanExecutorFailErrorCode = 4382101;
export const kPlanExecutorFailErrorCodeName = `Location${kPlanExecutorFailErrorCode}`;

/**
 * Server parameters that enable query stats error collection. Reads and writes are sampled by two
 * independent parameters, so each caller enables only what it needs:
 *  - read tests pass {writeSampleRate: 0},
 *  - write tests pass {readSampleRate: 0},
 *  - tests for the aggregate-based write stages ($out/$merge, which register under the READ rate)
 *    leave both at their default of 1.
 */
export function queryStatsOptions({
    readSampleRate = 1,
    writeSampleRate = 1,
    extraSetParameters = {},
} = {}) {
    return {
        setParameter: {
            ...getQueryStatsServerParameters().setParameter,
            internalQueryStatsSampleRate: readSampleRate,
            internalQueryStatsWriteCmdSampleRate: writeSampleRate,
            featureFlagQueryStatsErrors: true,
            ...extraSetParameters,
        },
    };
}

/**
 * Returns the count recorded for `errorCodeName` in `metrics.errors`, or 0 if absent.
 */
export function errorCountByName(metrics, errorCodeName) {
    const match = metrics?.errors?.find((e) => e.codeName === errorCodeName);
    return match?.count ?? 0;
}

/**
 * Returns the metrics of the most recent query stats entry for `collName`.
 */
export function getLatestQueryStatsMetrics(conn, collName) {
    const entry = getLatestQueryStatsEntry(conn, {collName});
    assert(entry.hasOwnProperty("metrics"), "missing metrics", {entry});
    return entry.metrics;
}

/**
 * Asserts the invariants for a shape that has only ever errored.
 */
export function assertOnlyErroredEntry(
    metrics,
    {execCountErrored, errorCodeName, errorCodeCount},
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
    assert.eq(
        errorCodeCount,
        errorCountByName(metrics, errorCodeName),
        `${errorDescription}: error count`,
        {
            metrics,
        },
    );
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
 * Runs `fn` with `failPoint` enabled on every connection in `conns`, always disabling it after.
 */
export function withFailPoint(conns, failPoint, data, fn) {
    const failPoints = conns.map((conn) => configureFailPoint(conn, failPoint, data ?? {}));
    try {
        fn();
    } finally {
        for (const fp of failPoints) {
            fp.off();
        }
    }
}

/**
 * Runs `fn` with `cmdNames` failing with `errorCode` on every connection in `conns`.
 * `failInternalCommands` is required because mongos reaches shards as an internal client.
 */
export function withCommandFailure(conns, cmdNames, errorCode, fn) {
    withFailPoint(
        conns,
        "failCommand",
        {errorCode: errorCode, failCommands: cmdNames, failInternalCommands: true},
        fn,
    );
}

/**
 * Asserts that running `action` fails with `expectedErrorCode`, whether it throws (shell CRUD
 * helpers) or returns a failed command result (runCommand).
 */
export function expectOperationFailure(action, ctx, expectedErrorCode, desc) {
    let result;
    let error;
    try {
        result = action(ctx);
    } catch (e) {
        error = e;
    }
    if (error !== undefined) {
        assert.eq(error.code, expectedErrorCode, `${desc}: unexpected error code`, {error});
    } else {
        assert.commandFailedWithCode(result, expectedErrorCode, desc);
    }
}

/**
 * Registers one `it` per case in `cases`. Each test resets state via `resetPerTest`, runs the
 * operation under its failpoint, and asserts the error was recorded against the shape. Registering
 * one test per case keeps each test point independent: deleting a row deletes exactly one test.
 *
 * Case shape:
 *   {
 *     name,                       // it() title
 *     action(ctx),                // performs the operation; throws or returns a failed command result
 *     induce: {failpoint, data},  // optional; enabled around action()
 *     expectedErrorCode,          // ErrorCodes value the operation must fail with
 *     expectedErrorCodeName,      // errorCodeName expected in metrics.errors
 *     skip,                       // optional; if true the case is registered with it.skip
 *     setup(ctx),                 // optional per-case prep, run before the failpoint
 *     expectedExecCountErrored, expectedErrorCodeCount,  // optional overrides
 *   }
 *
 * `hooks` supplies:
 *   resetPerTest()        -- reset store (+ collection where writes may mutate it) for THIS case
 *   getCtx()              -- {conn, testDB, coll, collName}
 *   targetConnections(c)  -- connections to enable the failpoint on
 *   getMetrics(ctx, c)    -- the metrics to assert on
 */
export function describeErroredOpCases(cases, hooks) {
    for (const testCase of cases) {
        const body = function () {
            hooks.resetPerTest();
            const ctx = hooks.getCtx();
            if (testCase.setup) {
                testCase.setup(ctx);
            }
            const run = () =>
                expectOperationFailure(
                    testCase.action,
                    ctx,
                    testCase.expectedErrorCode,
                    testCase.name,
                );
            if (testCase.induce) {
                withFailPoint(
                    hooks.targetConnections(testCase),
                    testCase.induce.failpoint,
                    testCase.induce.data,
                    run,
                );
            } else {
                run();
            }
            assertOnlyErroredEntry(
                hooks.getMetrics(ctx, testCase),
                {
                    execCountErrored: testCase.expectedExecCountErrored ?? 1,
                    errorCodeName: testCase.expectedErrorCodeName,
                    errorCodeCount: testCase.expectedErrorCodeCount ?? 1,
                },
                testCase.name,
            );
        };
        // `skip: true` marks a case the server cannot satisfy yet (see the case's own comment); it is
        // registered with it.skip so it is honestly reported as untested rather than silently
        // passing.
        if (testCase.skip) {
            it.skip(testCase.name, body);
        } else {
            it(testCase.name, body);
        }
    }
}
