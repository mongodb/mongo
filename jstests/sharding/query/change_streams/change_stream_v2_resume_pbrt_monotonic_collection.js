/**
 * Resumes a sharded collection change stream from an event token, a high-water-mark token, and an invalidate token, and asserts the initial postBatchResumeToken never regresses.
 *
 * @tags: [
 *   # Asserts on PBRT monotonicity at collection scope specifically; breaks once upconverted.
 *   do_not_run_in_whole_db_passthrough,
 *   do_not_run_in_whole_cluster_passthrough,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {describe, before, afterEach, after} from "jstests/libs/mochalite.js";
import {ChangeStreamTest, ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    kDataDistributions,
    runChainedResumePbrtMonotonicTest,
    runControlEventPbrtResumableTest,
    runResumePbrtMonotonicTests,
    setupShardedCollection,
} from "jstests/libs/query/change_stream_resume_pbrt_monotonic_helpers.js";

describe("collection-level change stream resume token monotonicity", function () {
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

        ctx.db = st.s.getDB(jsTestName());
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

    function getCollectionInvalidateToken({ctx, distribution}) {
        const invalidateColl = ctx.db.getCollection(ctx.coll.getName() + "_invalidate");
        assertDropCollection(ctx.db, invalidateColl.getName());
        setupShardedCollection({db: ctx.db, coll: invalidateColl, st});

        const cursor = ctx.cst.getChangeStream({
            watchMode: ChangeStreamWatchMode.kCollection,
            coll: invalidateColl,
        });

        const docs = kDataDistributions[distribution];
        assert.commandWorked(invalidateColl.insert(docs));
        const consumed = ctx.cst.getNextChanges(cursor, docs.length);
        assert.eq(consumed.length, docs.length, "did not consume all inserted events", {
            consumed,
            docs,
        });

        assertDropCollection(ctx.db, invalidateColl.getName());
        const dropEvent = ctx.cst.getOneChange(cursor);
        assert.eq(dropEvent.operationType, "drop", "expected a drop event before invalidate", {
            dropEvent,
        });
        const invalidate = ctx.cst.getOneChange(cursor, true /* expectInvalidate */);
        assert.eq(invalidate.operationType, "invalidate", "expected an invalidate event", {
            invalidate,
        });

        // Recreate the collection so that resuming from the invalidate token has a namespace to
        // attach to.
        assertCreateCollection(ctx.db, invalidateColl.getName());

        return {token: invalidate._id, resumeColl: invalidateColl};
    }

    runResumePbrtMonotonicTests({
        ctx,
        watchMode: ChangeStreamWatchMode.kCollection,
        getInvalidateToken: getCollectionInvalidateToken,
    });

    describe("collection-level chained resume from PBRT", function () {
        runChainedResumePbrtMonotonicTest({
            ctx,
            watchMode: ChangeStreamWatchMode.kCollection,
        });
    });

    runControlEventPbrtResumableTest({
        ctx,
        watchMode: ChangeStreamWatchMode.kCollection,
    });
});
