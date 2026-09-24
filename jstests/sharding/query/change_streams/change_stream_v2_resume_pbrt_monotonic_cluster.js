/**
 * Resumes a whole cluster change stream from an event token, a high-water-mark token,
 * and an invalidate token, and asserts the initial postBatchResumeToken never regresses.
 *
 * @tags: [
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
    runChainedResumePbrtMonotonicTest,
    runControlEventPbrtResumableTest,
    runResumePbrtMonotonicTests,
    setupShardedCollection,
} from "jstests/libs/query/change_stream_resume_pbrt_monotonic_helpers.js";

describe("whole-cluster change stream resume token monotonicity", function () {
    let st;
    const ctx = {db: null, adminDb: null, coll: null, cst: null};

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {
                enableBalancer: false,
            },
        });

        ctx.db = st.s.getDB(jsTestName());
        ctx.adminDb = st.s.getDB("admin");
        ctx.coll = ctx.db.test;
        ctx.st = st;

        setupShardedCollection({db: ctx.db, coll: ctx.coll, st});

        ctx.cst = new ChangeStreamTest(ctx.adminDb);
    });

    afterEach(function () {
        if (ctx.cst) {
            ctx.cst.cleanUp();
        }
    });

    after(function () {
        st.stop();
    });

    runResumePbrtMonotonicTests({
        ctx,
        watchMode: ChangeStreamWatchMode.kCluster,
    });

    runControlEventPbrtResumableTest({
        ctx,
        watchMode: ChangeStreamWatchMode.kCluster,
    });

    describe("whole-cluster chained resume from PBRT", function () {
        runChainedResumePbrtMonotonicTest({
            ctx,
            watchMode: ChangeStreamWatchMode.kCluster,
        });
    });
});
