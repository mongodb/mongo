/**
 * Tests that $queryStats records errors per query shape for read operations, on a mongod
 * (single-node replica set, so change streams are available) and on a sharded cluster.
 *
 * Only non-cursor read errors are collected: find/aggregate that error during the first batch, plus
 * count and distinct, which are inherently non-cursor. listDatabases, listCollections, listIndexes
 * and explain are not covered because they never register a query stats key.
 *
 * The aggregate-based write stages $out and $merge are covered here too (replica set only). They
 * register their key under the READ sampling rate, like any other aggregate, and their failpoints
 * fire before any internal insert batch runs, so the write sampling rate never comes into play.
 *
 * @tags: [featureFlagQueryStatsErrors, requires_sharding]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    describeErroredOpCases,
    errorCountByName,
    getLatestQueryStatsMetrics,
    kPlanExecutorFailErrorCode,
    kPlanExecutorFailErrorCodeName,
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
 * The aggregate-based write stages $out and $merge. They run inside the aggregate command, which
 * registers their key under the READ sampling rate even though they write; the write rate is never
 * consulted because their hang failpoints fire before any internal insert batch runs. The
 * failpoints are interruptible, so a short maxTimeMS kills them; runCommand is used so maxTimeMS is
 * definitely sent (the shell aggregate helper does not always forward it), avoiding an unbounded
 * hang. The recorded shape's command is "aggregate".
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
    function resetReadState() {
        coll.drop();
        assert.commandWorked(coll.insert(kSeedDocs));
        resetQueryStatsStore(conn, "1MB");
    }

    const replSetHooks = {
        getCtx: () => ({conn, testDB, coll, collName}),
        resetPerTest: resetReadState,
        targetConnections: () => [conn],
        getMetrics: (ctx) => getLatestQueryStatsMetrics(ctx.conn, ctx.collName),
    };

    describeErroredOpCases(readErrorCases(), replSetHooks);
    describeErroredOpCases(outMergeErrorCases(), replSetHooks);

    it("does not record errored fields on a successful execution", function () {
        resetReadState();
        assert.eq(2, coll.find({a: {$gte: 2}}).itcount());
        const metrics = getLatestQueryStatsMetrics(conn, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assert.eq(0, metrics.execCountErrored, "execCountErrored", {metrics});
        assert(!metrics.hasOwnProperty("errors"), "unexpected errors array", {metrics});
    });

    it("keeps successful timing when the same shape later errors", function () {
        resetReadState();
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
        assert.eq(1, metrics.execCountErrored, "execCountErrored", {metrics});
        assert.eq(1, errorCountByName(metrics, kPlanExecutorFailErrorCodeName), "error count", {
            metrics,
        });
        assert.eq(timingAfterSuccess, metrics.totalExecMicros.sum, "timing changed", {metrics});
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

    describeErroredOpCases(shardLevelReadCases(), hooks);
    describeErroredOpCases(routerLevelReadCases(), hooks);

    // The cases above all induce a failure. This guards the success path through mongos, where the
    // read fans out to every shard and none fails; nothing errored should be recorded.
    it("does not record errored fields on a successful execution", function () {
        resetQueryStatsStore(st.s, "1MB");
        assert.eq(2, coll.find({a: {$gte: 2}}).itcount());
        const metrics = getLatestQueryStatsMetrics(st.s, collName);
        assert.eq(1, metrics.execCount, "execCount", {metrics});
        assert.eq(0, metrics.execCountErrored, "execCountErrored", {metrics});
        assert(!metrics.hasOwnProperty("errors"), "unexpected errors array", {metrics});
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
                assert.eq(0, metrics.execCountErrored, `${name} execCountErrored`, {metrics});
                assert(
                    !metrics.hasOwnProperty("errors"),
                    `${name}: a partial-results success must not be recorded as errored`,
                    {metrics},
                );
            }
        });
    });
});
