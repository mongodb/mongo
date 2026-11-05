/**
 * Tests that $queryStats records errors per query shape for write operations, on a mongod
 * (single-node replica set) and on a sharded cluster.
 *
 * Only insert/update/delete register a query stats key on the write path; findAndModify and the
 * server bulkWrite command have no registration path and are therefore not covered here. The
 * aggregate-based write stages ($out/$merge) live in errored_reads.js because they register under
 * the READ sampling rate, not the write rate enabled here.
 *
 * @tags: [featureFlagQueryStatsErrors, requires_sharding]
 */
import {after, before, describe} from "jstests/libs/mochalite.js";
import {resetQueryStatsStore} from "jstests/libs/query/query_stats_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    describeErroredOpCases,
    getLatestQueryStatsMetrics,
    queryStatsOptions,
} from "jstests/noPassthrough/query/queryStats/query_stats_for_errors/utils.js";

const kSeedDocs = [
    {_id: 0, a: 1},
    {_id: 1, a: 2},
    {_id: 2, a: 3},
];

/**
 * Write operations that register a query stats key. insert/update/delete are induced to fail with
 * failAllInserts/failAllUpdates/failAllRemoves, which throw after the shape has been registered;
 * the deadline variants hang (interruptibly) and are killed by a short maxTimeMS.
 *
 * The update/delete cases are marked skip: true because update/delete errors are not recorded yet
 * (TODO SERVER-134987); only insert records today, so only the insert cases run.
 */
function writeErrorCases() {
    const internalError = {
        expectedErrorCode: ErrorCodes.InternalError,
        expectedErrorCodeName: "InternalError",
    };
    const maxTimeMSExpired = {
        expectedErrorCode: ErrorCodes.MaxTimeMSExpired,
        expectedErrorCodeName: "MaxTimeMSExpired",
    };
    return [
        {
            name: "insertOne",
            action: (ctx) => ctx.coll.insertOne({_id: 100, a: 1}),
            induce: {failpoint: "failAllInserts"},
            ...internalError,
        },
        {
            name: "insertMany",
            action: (ctx) => ctx.coll.insertMany([{_id: 101, a: 1}]),
            induce: {failpoint: "failAllInserts"},
            ...internalError,
        },
        {
            // runCommand is used so maxTimeMS is definitely sent; the batch hang is interruptible.
            name: "insertOne (deadline kill)",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    insert: ctx.collName,
                    documents: [{_id: 102, a: 1}],
                    maxTimeMS: 500,
                }),
            induce: {failpoint: "hangDuringBatchInsert", data: {shouldCheckForInterrupt: true}},
            ...maxTimeMSExpired,
        },
        {
            name: "updateOne",
            action: (ctx) => ctx.coll.updateOne({a: 1}, {$set: {b: 1}}),
            induce: {failpoint: "failAllUpdates"},
            ...internalError,
            skip: true,
        },
        {
            name: "updateMany",
            action: (ctx) => ctx.coll.updateMany({a: {$gte: 1}}, {$set: {b: 1}}),
            induce: {failpoint: "failAllUpdates"},
            ...internalError,
            skip: true,
        },
        {
            name: "replaceOne",
            action: (ctx) => ctx.coll.replaceOne({a: 1}, {a: 1, b: 2}),
            induce: {failpoint: "failAllUpdates"},
            ...internalError,
            skip: true,
        },
        {
            name: "updateOne (deadline kill)",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    update: ctx.collName,
                    updates: [{q: {a: 1}, u: {$set: {b: 1}}}],
                    maxTimeMS: 500,
                }),
            induce: {failpoint: "hangDuringBatchUpdate", data: {shouldCheckForInterrupt: true}},
            ...maxTimeMSExpired,
            skip: true,
        },
        {
            name: "deleteOne",
            action: (ctx) => ctx.coll.deleteOne({a: 1}),
            induce: {failpoint: "failAllRemoves"},
            ...internalError,
            skip: true,
        },
        {
            name: "deleteMany",
            action: (ctx) => ctx.coll.deleteMany({a: {$gte: 1}}),
            induce: {failpoint: "failAllRemoves"},
            ...internalError,
            skip: true,
        },
        {
            name: "deleteMany (deadline kill)",
            action: (ctx) =>
                ctx.testDB.runCommand({
                    delete: ctx.collName,
                    deletes: [{q: {a: {$gte: 1}}, limit: 0}],
                    maxTimeMS: 500,
                }),
            induce: {failpoint: "hangDuringBatchRemove", data: {shouldCheckForInterrupt: true}},
            ...maxTimeMSExpired,
            skip: true,
        },
    ];
}

/**
 * The subset of write cases exercised through mongos, where a single shard owns the data.
 */
function shardLevelWriteCases() {
    return writeErrorCases().filter((c) =>
        ["insertOne", "updateOne", "deleteOne"].includes(c.name),
    );
}

describe("query stats errored writes (replica set)", function () {
    let rst, conn, testDB, coll, collName;

    before(function () {
        rst = new ReplSetTest({nodes: 1, nodeOptions: queryStatsOptions({readSampleRate: 0})});
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

    describeErroredOpCases(writeErrorCases(), {
        getCtx: () => ({conn, testDB, coll, collName}),
        resetPerTest: () => {
            // Writes mutate the collection, so re-seed it, then reset the store so neither the seed
            // insert nor a previous case's writes appear in the assertions.
            coll.drop();
            assert.commandWorked(coll.insert(kSeedDocs));
            resetQueryStatsStore(conn, "1MB");
        },
        targetConnections: () => [conn],
        getMetrics: (ctx) => getLatestQueryStatsMetrics(ctx.conn, ctx.collName),
    });
});

describe("query stats errored writes (mongos)", function () {
    let st, testDB, coll, collName;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: queryStatsOptions({readSampleRate: 0}),
            rsOptions: queryStatsOptions({readSampleRate: 0}),
        });
        testDB = st.getDB("test");
        collName = jsTestName();
        coll = testDB[collName];
        coll.drop();
        // Pin shard0 as the primary shard and shard on {_id: 1} WITHOUT splitting, so a single
        // chunk (and every document) lives on shard0. This makes the writes below deterministically
        // target shard0, where the failpoint is configured. (enableSharding without an explicit
        // primaryShard can pick either shard, and passing a split point to shardColl also defaults
        // the move target, moving a chunk off shard0.)
        assert.commandWorked(
            testDB.adminCommand({
                enableSharding: testDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(coll.insert(kSeedDocs));
    });

    after(function () {
        st?.stop();
    });

    describeErroredOpCases(shardLevelWriteCases(), {
        getCtx: () => ({conn: st.s, testDB, coll, collName}),
        resetPerTest: () => {
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(coll.insert(kSeedDocs));
            resetQueryStatsStore(st.s, "1MB");
            resetQueryStatsStore(st.shard0, "1MB");
        },
        targetConnections: () => [st.shard0],
        // The error is recorded on the shard that executed the write.
        getMetrics: (ctx) => getLatestQueryStatsMetrics(st.shard0, ctx.collName),
    });
});
