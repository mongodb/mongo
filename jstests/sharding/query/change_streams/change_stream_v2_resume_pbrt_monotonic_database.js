/**
 * Resumes a sharded database change stream from an event token, a high-water-mark token, and an invalidate token, and asserts the initial postBatchResumeToken never regresses.
 *
 * @tags: [
 *   # Asserts on PBRT monotonicity at database scope specifically; breaks once upconverted.
 *   do_not_run_in_whole_db_passthrough,
 *   do_not_run_in_whole_cluster_passthrough,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {describe, before, afterEach, after} from "jstests/libs/mochalite.js";
import {ChangeStreamTest, ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    kDataDistributions,
    runChainedResumePbrtMonotonicTest,
    runControlEventPbrtResumableTest,
    runResumePbrtMonotonicTests,
    setupShardedCollection,
} from "jstests/libs/query/change_stream_resume_pbrt_monotonic_helpers.js";

describe("database-level change stream resume token monotonicity", function () {
    let st;
    const ctx = {db: null, coll: null, cst: null};

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {
                enableBalancer: false,
            },
        });

        ctx.db = st.s.getDB(jsTestName() + "_dbwatch");
        ctx.coll = ctx.db.test;
        ctx.st = st;

        setupShardedCollection({db: ctx.db, coll: ctx.coll, st});

        ctx.cst = new ChangeStreamTest(ctx.db);
    });

    afterEach(function () {
        if (ctx.cst) {
            ctx.cst.cleanUp();
        }
    });

    after(function () {
        st.stop();
    });

    function getDbInvalidateToken({ctx, distribution}) {
        const cursor = ctx.cst.getChangeStream({
            watchMode: ChangeStreamWatchMode.kDb,
            coll: 1,
        });

        // Use ids that do not overlap with the earlier event/high-watermark cases in this file.
        const docs =
            distribution === "oneShard"
                ? [{_id: -200, shard: 0}]
                : [
                      {_id: -201, shard: 0},
                      {_id: 2, shard: 1},
                  ];
        assert.commandWorked(ctx.coll.insert(docs));
        const consumed = ctx.cst.getNextChanges(cursor, docs.length);
        assert.eq(consumed.length, docs.length, "did not consume all inserted events", {
            consumed,
            docs,
        });

        assert.commandWorked(ctx.db.dropDatabase());
        ctx.cst.assertDatabaseDrop({cursor, db: ctx.db});
        const invalidate = ctx.cst.getOneChange(cursor, true /* expectInvalidate */);
        assert.eq(invalidate.operationType, "invalidate", "expected an invalidate event", {
            invalidate,
        });

        // Recreate the watched database so that resuming from the invalidate token can open.
        setupShardedCollection({db: ctx.db, coll: ctx.coll, st});

        return invalidate._id;
    }

    runResumePbrtMonotonicTests({
        ctx,
        watchMode: ChangeStreamWatchMode.kDb,
        getInvalidateToken: getDbInvalidateToken,
    });

    describe("database-level chained resume from PBRT", function () {
        runChainedResumePbrtMonotonicTest({
            ctx,
            watchMode: ChangeStreamWatchMode.kDb,
        });
    });

    runControlEventPbrtResumableTest({
        ctx,
        watchMode: ChangeStreamWatchMode.kDb,
    });
});
