/**
 * Tests that $queryStats records errors per query shape for read operations, on a mongod and on a
 * sharded cluster.
 *
 * @tags: [featureFlagQueryStatsErrors]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertErrorRecorded,
    assertNoErrorRecorded,
    describeOnlyErroredOpCases,
    getLatestQueryStatsMetrics,
    kPlanExecutorFailErrorCode,
    kPlanExecutorFailErrorCodeName,
    killCursorWhileIdle,
    openFindCursor,
    queryStatsOptions,
    withCommandFailure,
    withFailPoint,
} from "jstests/noPassthrough/query/queryStats/query_stats_for_errors/utils.js";

const kSeedDocs = [
    {_id: 0, a: 1},
    {_id: 1, a: 2},
    {_id: 2, a: 3},
];

/**
 * Read operations that register a query stats key and can therefore record an error. Each is
 * induced to fail with a plan executor throw.
 */
function readErrorCases() {
    const planExecutorFails = {failpoint: "planExecutorAlwaysFails"};
    return [
        {
            name: "find",
            action: (ctx) => ctx.coll.find({a: {$gte: 0}}).itcount(),
            induce: planExecutorFails,
            expectedErrorCode: kPlanExecutorFailErrorCode,
            expectedErrorCodeName: kPlanExecutorFailErrorCodeName,
        },
        {
            name: "findOne",
            action: (ctx) => ctx.coll.findOne({a: {$gte: 0}}),
            induce: planExecutorFails,
            expectedErrorCode: kPlanExecutorFailErrorCode,
            expectedErrorCodeName: kPlanExecutorFailErrorCodeName,
        },
        {
            name: "aggregate",
            action: (ctx) => ctx.coll.aggregate([{$match: {a: {$gte: 0}}}]).toArray(),
            induce: planExecutorFails,
            expectedErrorCode: kPlanExecutorFailErrorCode,
            expectedErrorCodeName: kPlanExecutorFailErrorCodeName,
        },
        {
            name: "distinct",
            action: (ctx) => ctx.coll.distinct("a", {a: {$gte: 0}}),
            induce: planExecutorFails,
            expectedErrorCode: kPlanExecutorFailErrorCode,
            expectedErrorCodeName: kPlanExecutorFailErrorCodeName,
        },
        {
            name: "countDocuments",
            action: (ctx) => ctx.coll.countDocuments({a: {$gte: 0}}),
            induce: planExecutorFails,
            expectedErrorCode: kPlanExecutorFailErrorCode,
            expectedErrorCodeName: kPlanExecutorFailErrorCodeName,
        },
        {
            name: "count",
            action: (ctx) => ctx.coll.count({a: {$gte: 0}}),
            induce: planExecutorFails,
            expectedErrorCode: kPlanExecutorFailErrorCode,
            expectedErrorCodeName: kPlanExecutorFailErrorCodeName,
        },
        {
            name: "find (deadline kill)",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    find: ctx.collName,
                    filter: {a: {$gte: 0}},
                    maxTimeMS: 500,
                }),
            induce: {
                failpoint: "waitInFindBeforeMakingBatch",
                data: {shouldCheckForInterrupt: true},
            },
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
        },
        {
            // Change streams require a replica set. throwErrorBeforeGetNext fires for change-stream
            // resumable scans. runCommand (rather than coll.watch().next()) with maxTimeMS is used
            // so that if the failpoint does not fire the command returns instead of blocking
            // forever.
            name: "watch",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    aggregate: ctx.collName,
                    pipeline: [{$changeStream: {}}],
                    cursor: {},
                    maxTimeMS: 500,
                }),
            induce: {failpoint: "throwErrorBeforeGetNext", data: {code: ErrorCodes.InternalError}},
            expectedErrorCode: ErrorCodes.InternalError,
            expectedErrorCodeName: "InternalError",
        },
    ];
}

/**
 * $out and $merge run inside the aggregate command, which register their key under the read sampling rate.
 * Even though they write, the write rate is never consulted because their hang failpoints fire before any
 * internal insert batch runs.
 */
function outMergeErrorCases() {
    return [
        {
            name: "$out",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    aggregate: ctx.collName,
                    pipeline: [{$match: {a: {$gte: 0}}}, {$out: "out_target"}],
                    cursor: {},
                    maxTimeMS: 500,
                }),
            induce: {
                failpoint: "hangWhileBuildingDocumentSourceOutBatch",
                data: {shouldCheckForInterrupt: true},
            },
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
        },
        {
            name: "$merge",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    aggregate: ctx.collName,
                    pipeline: [{$match: {a: {$gte: 0}}}, {$merge: {into: "merge_target"}}],
                    cursor: {},
                    maxTimeMS: 500,
                }),
            induce: {
                failpoint: "hangWhileBuildingDocumentSourceMergeBatch",
                data: {shouldCheckForInterrupt: true},
            },
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
        },
    ];
}

/**
 * getMores that fail on a find or aggregate cursor. The error is recorded on the cursor and written
 * when the cursor is disposed. A cursor that survives the error is disposed in 'afterAction'.
 */
function cursorGetMoreErrorCases() {
    return [
        {
            name: "getMore on an aggregate cursor",
            setup: (ctx) => {
                const res = assert.commandWorked(
                    ctx.testDB.runCommand({
                        aggregate: ctx.collName,
                        pipeline: [{$match: {a: {$gte: 0}}}],
                        cursor: {batchSize: 1},
                    }),
                );
                assert.neq(0, res.cursor.id, "expected a cursor to remain open", {res});
                ctx.cursorId = res.cursor.id;
            },
            action: (ctx) =>
                ctx.testDB.runCommand({getMore: ctx.cursorId, collection: ctx.collName}),
            induce: {
                failpoint: "failGetMoreAfterCursorCheckout",
                data: {errorCode: ErrorCodes.MaxTimeMSExpired},
            },
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
        },
        {
            // Fails because the getMore waits just before generating its batch until its deadline
            // expires. We set the deadline on the find because a getMore on a non-awaitData cursor
            // can't set its own 'maxTimeMS'.
            name: "getMore killed by its deadline",
            setup: (ctx) => {
                ctx.cursorId = openFindCursor(ctx.testDB, ctx.collName, {maxTimeMS: 500});
            },
            action: (ctx) =>
                ctx.testDB.runCommand({getMore: ctx.cursorId, collection: ctx.collName}),
            induce: {
                failpoint: "waitWithPinnedCursorDuringGetMoreBatch",
                data: {shouldCheckForInterrupt: true},
            },
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
        },
        {
            name: "getMore that fails before it iterates the cursor",
            setup: (ctx) => {
                ctx.cursorId = openFindCursor(ctx.testDB, ctx.collName);
            },
            action: (ctx) =>
                ctx.testDB.runCommand({getMore: ctx.cursorId, collection: ctx.collName}),
            induce: {
                failpoint: "failGetMoreDuringCursorLockAcquisition",
                data: {errorCode: ErrorCodes.QueryPlanKilled},
            },
            expectedErrorCode: ErrorCodes.QueryPlanKilled,
            expectedErrorCodeName: "QueryPlanKilled",
        },
        {
            // Fails while inheriting the cursor's settings, before any batch work starts.
            name: "getMore that fails before its batch is generated",
            setup: (ctx) => {
                ctx.cursorId = openFindCursor(ctx.testDB, ctx.collName);
            },
            action: (ctx) =>
                ctx.testDB.runCommand({
                    getMore: ctx.cursorId,
                    collection: ctx.collName,
                    apiVersion: "1",
                    apiStrict: false,
                }),
            afterAction: (ctx) =>
                assert.commandWorked(
                    ctx.testDB.runCommand({killCursors: ctx.collName, cursors: [ctx.cursorId]}),
                ),
            expectedErrorCode: ErrorCodes.APIMismatchError,
            expectedErrorCodeName: "APIMismatchError",
        },
        {
            name: "getMore that fails after its batch is generated",
            setup: (ctx) => {
                ctx.cursorId = openFindCursor(ctx.testDB, ctx.collName);
            },
            action: (ctx) =>
                ctx.testDB.runCommand({
                    getMore: ctx.cursorId,
                    collection: ctx.collName,
                    batchSize: 1,
                }),
            induce: {
                failpoint: "failGetMoreAfterBatchGeneration",
                data: {errorCode: ErrorCodes.Interrupted},
            },
            afterAction: (ctx) =>
                assert.commandWorked(
                    ctx.testDB.runCommand({killCursors: ctx.collName, cursors: [ctx.cursorId]}),
                ),
            expectedErrorCode: ErrorCodes.Interrupted,
            expectedErrorCodeName: "Interrupted",
        },
        {
            // The cursor is disposed while being pinned for the failing getMore, before it can
            // iterate.
            name: "getMore on a cursor that was killed while idle",
            setup: (ctx) => {
                ctx.cursorId = openFindCursor(ctx.testDB, ctx.collName, {maxTimeMS: 2000});
                killCursorWhileIdle(ctx.conn, ctx.testDB, ctx.collName, ctx.cursorId);
            },
            action: (ctx) =>
                ctx.testDB.runCommand({getMore: ctx.cursorId, collection: ctx.collName}),
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
        },
    ];
}

describe("query stats errored reads (replica set)", function () {
    let rst, conn, testDB, coll, collName;

    before(function () {
        rst = new ReplSetTest({nodes: 1, nodeOptions: queryStatsOptions({writeSampleRate: 0})});
        rst.startSet();
        rst.initiate();
        conn = rst.getPrimary();
        testDB = conn.getDB("test");
        collName = jsTestName();
        coll = testDB[collName];
    });

    after(function () {
        rst?.stopSet();
    });

    // Drop and re-seed the collection, then reset the store so the seed insert does not appear as a
    // query stats entry. Each test point starts from this clean state.
    function resetCollectionAndQueryStatsStore() {
        coll.drop();
        assert.commandWorked(coll.insert(kSeedDocs));
        resetQueryStatsStore(conn, "1MB");
    }

    const replSetHooks = {
        getCtx: () => ({conn, testDB, coll, collName}),
        resetPerTest: resetCollectionAndQueryStatsStore,
        targetConnections: () => [conn],
        getMetrics: (ctx) => getLatestQueryStatsMetrics(ctx.conn, ctx.collName),
    };

    describeOnlyErroredOpCases(readErrorCases(), replSetHooks);
    describeOnlyErroredOpCases(outMergeErrorCases(), replSetHooks);
    describeOnlyErroredOpCases(cursorGetMoreErrorCases(), replSetHooks);

    // The cases below don't fit the describeOnlyErroredOpCases fixture. Each either succeeds, runs
    // a success on the shape first, or expects no error to be recorded.
    it("does not record errored fields on a successful execution", function () {
        resetCollectionAndQueryStatsStore();
        assert.eq(2, coll.find({a: {$gte: 2}}).itcount());
        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assertNoErrorRecorded(metrics, "successful execution");
    });

    it("keeps successful timing when the same shape later errors", function () {
        resetCollectionAndQueryStatsStore();
        const filter = {a: {$gte: 0}};
        assert.commandWorked(testDB.runCommand({find: collName, filter}));
        const timingAfterSuccess = getLatestQueryStatsMetrics(conn, collName).totalExecMicros.sum;
        assert.gt(timingAfterSuccess, 0, "success should record timing");

        // The same shape now errors, recording on the same entry.
        withFailPoint([conn], "planExecutorAlwaysFails", {}, () => {
            assert.commandFailedWithCode(
                testDB.runCommand({find: collName, filter}),
                kPlanExecutorFailErrorCode,
            );
        });

        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assertErrorRecorded(
            metrics,
            {execCountErrored: 1, errorCodeName: kPlanExecutorFailErrorCodeName, errorCodeCount: 1},
            "error after a success",
        );
        assert.eq(timingAfterSuccess, metrics.totalExecMicros.sum, "timing changed", {metrics});
    });

    it("does not record an error when the client closes a cursor early", function () {
        resetCollectionAndQueryStatsStore();
        // Closing a cursor early is a normal client choice.
        const cursorId = openFindCursor(testDB, collName);
        assert.commandWorked(testDB.runCommand({killCursors: collName, cursors: [cursorId]}));

        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assertNoErrorRecorded(metrics, "successful execution");
    });

    it("does not record a killed idle cursor reached by a request from another session", function () {
        resetCollectionAndQueryStatsStore();
        // The cursor belongs to an explicit session and dies while idle. A getMore from outside
        // that session is not allowed to use the cursor, but still gets the error saying why it
        // died. That error must not be recorded against the owner's query, since the owner never
        // sent a failing request.
        const session = conn.startSession();
        const sessionDB = session.getDatabase(testDB.getName());
        try {
            const cursorId = openFindCursor(sessionDB, collName, {maxTimeMS: 2000});
            killCursorWhileIdle(conn, sessionDB, collName, cursorId);

            assert.commandFailedWithCode(
                testDB.runCommand({getMore: cursorId, collection: collName}),
                ErrorCodes.MaxTimeMSExpired,
                "expected the getMore to report why the cursor was killed",
            );

            assertNoErrorRecorded(getLatestQueryStatsMetrics(conn, collName), "another session");
        } finally {
            session.endSession();
        }
    });

    it("does not record an error for a cursor killed while idle that releaseMemory reports", function () {
        resetCollectionAndQueryStatsStore();
        // releaseMemory reports the cursor's kill reason in 'cursorsWithErrors' but returns ok: 1, so
        // the client never sees a failed command and nothing should be recorded as errored.
        const cursorId = openFindCursor(testDB, collName, {maxTimeMS: 2000});
        killCursorWhileIdle(conn, testDB, collName, cursorId);

        const releaseRes = assert.commandWorked(testDB.runCommand({releaseMemory: [cursorId]}));
        assert.eq(
            1,
            releaseRes.cursorsWithErrors.length,
            "expected the kill reason to be reported",
            {releaseRes},
        );
        assert.eq(
            ErrorCodes.MaxTimeMSExpired,
            releaseRes.cursorsWithErrors[0].status.code,
            "expected the reported error to be the kill reason",
            {releaseRes},
        );

        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        // An ok:1 releaseMemory must not be recorded as errored.
        assertNoErrorRecorded(metrics, "releaseMemory");
    });
});

/**
 * Shard-level read failures: one of the two targeted shards fails the read command.
 */
function shardLevelReadCases() {
    const commands = {
        find: (collName) => ({find: collName, filter: {findErr: {$gte: 0}}}),
        aggregate: (collName) => ({
            aggregate: collName,
            pipeline: [{$match: {aggErr: {$gte: 0}}}],
            cursor: {},
        }),
        count: (collName) => ({count: collName, query: {countErr: {$gte: 0}}}),
        distinct: (collName) => ({distinct: collName, key: "distinctErr"}),
    };
    return Object.keys(commands).map((name) => ({
        name: `shard-level ${name}`,
        action: (ctx) => ctx.testDB.runCommand(commands[name](ctx.collName)),
        induce: {
            failpoint: "failCommand",
            data: {
                errorCode: ErrorCodes.MaxTimeMSExpired,
                failCommands: [name],
                failInternalCommands: true,
            },
        },
        expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
        expectedErrorCodeName: "MaxTimeMSExpired",
        target: "shard0",
    }));
}

/**
 * Router-level read failures. The error originates in mongos, either validating the request before
 * contacting shards, validating a shard's response, or via the router's own deadline.
 */
function routerLevelReadCases() {
    return [
        {
            // Projecting the reserved $sortKey field is rejected by mongos.
            name: "router validation: $sortKey projection",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    find: ctx.collName,
                    filter: {routerSortKey: {$gte: 0}},
                    projection: {$sortKey: 1},
                }),
            expectedErrorCode: ErrorCodes.BadValue,
            expectedErrorCodeName: "BadValue",
            target: "router",
        },
        {
            name: "router response validation: find",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    find: ctx.collName,
                    filter: {routerValidationFind: {$gte: 0}},
                }),
            induce: {failpoint: "throwDuringCursorResponseValidation"},
            expectedErrorCode: ErrorCodes.FailedToParse,
            expectedErrorCodeName: "FailedToParse",
            target: "router",
        },
        {
            name: "router response validation: aggregate",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    aggregate: ctx.collName,
                    pipeline: [{$match: {routerValidationAgg: {$gte: 0}}}],
                    cursor: {},
                }),
            induce: {failpoint: "throwDuringCursorResponseValidation"},
            expectedErrorCode: ErrorCodes.FailedToParse,
            expectedErrorCodeName: "FailedToParse",
            target: "router",
        },
        {
            // Shards cannot raise anything (shouldCheckForInterrupt: false), so MaxTimeMSExpired is
            // the router's own deadline.
            name: "router deadline kill: find",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    find: ctx.collName,
                    filter: {routerTimeout: {$lte: 100}},
                    maxTimeMS: 500,
                }),
            induce: {
                failpoint: "shardWaitInFindBeforeMakingBatch",
                data: {shouldCheckForInterrupt: false},
            },
            expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
            expectedErrorCodeName: "MaxTimeMSExpired",
            target: "allShards",
        },
    ];
}

describe("query stats errored reads (mongos)", function () {
    let st, testDB, coll, collName;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: queryStatsOptions({writeSampleRate: 0}),
            rsOptions: queryStatsOptions({writeSampleRate: 0}),
        });
        testDB = st.getDB("test");
        collName = jsTestName();
        coll = testDB[collName];
        coll.drop();
        // Data lives across both shards so reads target more than one shard.
        st.shardColl(coll, {_id: 1}, {_id: 1}, {_id: 1});
        assert.commandWorked(coll.insert(kSeedDocs));
    });

    after(function () {
        st?.stop();
    });

    const targetConnections = (testCase) => {
        switch (testCase.target) {
            case "router":
                return [st.s];
            case "allShards":
                return [st.shard0, st.shard1];
            case "shard0":
            default:
                return [st.shard0];
        }
    };

    const hooks = {
        getCtx: () => ({conn: st.s, testDB, coll, collName}),
        resetPerTest: () => resetQueryStatsStore(st.s, "1MB"),
        targetConnections,
        getMetrics: (ctx) => getLatestQueryStatsMetrics(ctx.conn, ctx.collName),
    };

    describeOnlyErroredOpCases(shardLevelReadCases(), hooks);
    describeOnlyErroredOpCases(routerLevelReadCases(), hooks);

    // The cases below don't fit describeOnlyErroredOpCases because the operation succeeds.

    // The cases above all induce a failure. This guards the success path through mongos, where the
    // read fans out to every shard and none fails; nothing errored should be recorded.
    it("does not record errored fields on a successful execution", function () {
        resetQueryStatsStore(st.s, "1MB");
        assert.eq(2, coll.find({a: {$gte: 2}}).itcount());
        const metrics = getLatestQueryStatsMetrics(st.s, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assertNoErrorRecorded(metrics, "successful execution");
    });

    // allowPartialResults lets find/aggregate survive a failing shard, so the operation succeeds
    // and nothing errored should be recorded.
    describe("allowPartialResults", function () {
        it("does not record an error when a shard fails but partial results are returned", function () {
            const cases = [
                {
                    name: "find",
                    cmd: {
                        find: collName,
                        filter: {partialFind: {$gte: 0}},
                        allowPartialResults: true,
                    },
                },
                {
                    name: "aggregate",
                    cmd: {
                        aggregate: collName,
                        pipeline: [{$match: {partialAgg: {$gte: 0}}}],
                        cursor: {},
                        allowPartialResults: true,
                    },
                },
            ];

            for (const {name, cmd} of cases) {
                resetQueryStatsStore(st.s, "1MB");
                withCommandFailure([st.shard0], [name], ErrorCodes.MaxTimeMSExpired, () => {
                    const res = assert.commandWorked(testDB.runCommand(cmd));
                    assert(
                        res.cursor.partialResultsReturned,
                        `expected ${name} to return partial results`,
                        {res},
                    );
                });

                const metrics = getLatestQueryStatsMetrics(st.s, collName);
                assert.eq(1, metrics.execCount, `${name} execCount`, {metrics});
                // A partial-results success must not be recorded as errored.
                assertNoErrorRecorded(metrics, `${name} with partial results`);
            }
        });
    });
});

/**
 * Change stream getMores exercise paths the find/aggregate cases don't reach. A change stream
 * writes an entry on the initial command and again on every successful getMore, so when one fails
 * its key is already gone and the error is written without it. There are also change-stream-only
 * checks that kill the cursor before the usual getMore error handling, so each records its error
 * itself.
 *
 * These tests are separated from those above because the shape always has a prior success and
 * the step-down case needs its own two-node replica set.
 */
describe("query stats errored getMores on change stream cursors", function () {
    let rst, conn, testDB, coll, collName;

    before(function () {
        // Two nodes so that the last case can step the primary down.
        rst = new ReplSetTest({nodes: 2, nodeOptions: queryStatsOptions({writeSampleRate: 0})});
        rst.startSet();
        rst.initiate();
        conn = rst.getPrimary();
        testDB = conn.getDB("test");
        collName = jsTestName();
        coll = testDB[collName];
        coll.drop();
        assert.commandWorked(coll.insert({_id: 0}));
    });

    beforeEach(function () {
        resetQueryStatsStore(conn, "1MB");
    });

    after(function () {
        rst.stopSet();
    });

    function openChangeStream() {
        const res = assert.commandWorked(
            testDB.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {}}],
                cursor: {},
            }),
        );
        assert.neq(0, res.cursor.id, "expected a change stream cursor", {res});
        return res.cursor.id;
    }

    it("keeps successful metrics when a later getMore errors", function () {
        const cursorId = openChangeStream();

        assert.eq(
            1,
            getLatestQueryStatsMetrics(conn, collName).execCount,
            "establishment should record an execution",
        );
        assert.commandWorked(
            testDB.runCommand({getMore: cursorId, collection: collName, maxTimeMS: 500}),
        );
        let metrics = getLatestQueryStatsMetrics(conn, collName);
        assert.eq(2, metrics.execCount, "successful getMore", {metrics});
        const timingAfterSuccess = metrics.totalExecMicros.sum;
        assert.gt(timingAfterSuccess, 0, "successes should record timing", {metrics});

        const fp = configureFailPoint(testDB, "failGetMoreAfterCursorCheckout", {
            errorCode: ErrorCodes.Interrupted,
        });
        try {
            assert.commandFailedWithCode(
                testDB.runCommand({getMore: cursorId, collection: collName, maxTimeMS: 500}),
                ErrorCodes.Interrupted,
                "expected the change stream getMore to fail",
            );
        } finally {
            fp.off();
        }

        metrics = getLatestQueryStatsMetrics(conn, collName);
        assertErrorRecorded(
            metrics,
            {execCountErrored: 1, errorCodeName: "Interrupted", errorCodeCount: 1},
            "change stream getMore failure",
        );
        assert.eq(2, metrics.execCount, "the failure must not count as an execution", {metrics});
        assert.eq(
            timingAfterSuccess,
            metrics.totalExecMicros.sum,
            "an error must not disturb the successful metrics",
            {metrics},
        );
    });

    it("records a change stream killed after its optimized lookup is disabled", function () {
        // A stream built on the optimized lookup path is killed once that path is turned off.
        const res = assert.commandWorked(
            testDB.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                cursor: {batchSize: 0},
            }),
        );
        assert.neq(0, res.cursor.id, "expected a change stream cursor", {res});

        const flag = "featureFlagChangeStreamOptimizedUpdateLookup";
        assert.commandWorked(conn.adminCommand({setParameter: 1, [flag]: false}));
        try {
            assert.commandFailedWithCode(
                testDB.runCommand({getMore: res.cursor.id, collection: collName}),
                ErrorCodes.RetryChangeStream,
                "expected the stream to be killed",
            );
        } finally {
            assert.commandWorked(conn.adminCommand({setParameter: 1, [flag]: true}));
        }

        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assertErrorRecorded(
            metrics,
            {execCountErrored: 1, errorCodeName: "RetryChangeStream", errorCodeCount: 1},
            "change stream killed after optimized lookup disabled",
        );
    });

    // Must stay last in this block, since it changes which node is primary.
    it("records a change stream killed after its primary steps down", function () {
        // A stream opened with readPreference primary can no longer be served once that node
        // steps down.
        const res = assert.commandWorked(
            testDB.runCommand({
                aggregate: collName,
                pipeline: [{$changeStream: {}}],
                cursor: {},
                $readPreference: {mode: "primary"},
            }),
        );
        assert.neq(0, res.cursor.id, "expected a change stream cursor", {res});

        assert.commandWorked(conn.adminCommand({replSetStepDown: 60, force: true}));
        rst.awaitSecondaryNodes(null, [conn]);

        assert.commandFailedWithCode(
            testDB.runCommand({getMore: res.cursor.id, collection: collName}),
            ErrorCodes.InterruptedDueToReplStateChange,
            "expected the stream to be killed after the step-down",
        );

        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assertErrorRecorded(
            metrics,
            {
                execCountErrored: 1,
                errorCodeName: "InterruptedDueToReplStateChange",
                errorCodeCount: 1,
            },
            "change stream killed after step-down",
        );
    });
});
