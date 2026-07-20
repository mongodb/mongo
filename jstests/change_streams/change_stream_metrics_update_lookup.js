/**
 * Verifies that enriching update events with a post-image (fullDocument: "updateLookup") records the
 * single-document-lookup outcome into the per-engine serverStatus metrics under
 * 'changeStreams.updateLookup.<engine>'.
 *
 * @tags: [
 *   # Can not run in balancer suites as it expects no data migrations across shards for correct
 *   # metric capture (test expects shard local lookup).
 *   assumes_balancer_off,
 *   requires_fcv_90,
 *   assumes_no_implicit_cursor_exhaustion,
 *   # The 'gone' document's post-image lookup relies on observing the delete that immediately
 *   # follows its update. The lookup's read concern is {level: "majority", afterClusterTime:
 *   # <the update's own clusterTime T1>}, a minimum bound, not "now". A secondary servicing
 *   # that read may not yet have majority-committed (or advanced its per-batch lastApplied past)
 *   # the later delete at T2 > T1, so it can legitimately still see the pre-delete state.
 *   assumes_read_preference_unchanged,
 * ]
 */
import {before, beforeEach, after, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    ChangeStreamWatchMode,
    changeStreamPassthroughType,
    withChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {
    ServerStatusMetrics,
    UpdateLookupExecutor,
} from "jstests/libs/query/change_stream_metrics_util.js";

// The engine expected to handle an updateLookup given the optimized-updateLookup flag state. When
// the flag is off, the Aggregation executor is the entire lookup path regardless of topology. When
// it's on: collection-level streams have a fixed lookup namespace and use the caching SBE executor;
// db/cluster-level streams look up a different namespace per event and use Express.
function expectedEngine(isRunningOptimizedUpdateLookup) {
    if (!isRunningOptimizedUpdateLookup) {
        return UpdateLookupExecutor.kAggregation;
    }
    return changeStreamPassthroughType() === ChangeStreamWatchMode.kCollection
        ? UpdateLookupExecutor.kSBE
        : UpdateLookupExecutor.kExpress;
}

describe("change stream updateLookup single-document-lookup metrics", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const testColl = testDB.getCollection("test");
    let isRunningOptimizedUpdateLookup;

    before(function () {
        isRunningOptimizedUpdateLookup = FeatureFlagUtil.isEnabled(
            testDB,
            "ChangeStreamOptimizedUpdateLookup",
        );

        assertDropAndRecreateCollection(testDB, testColl.getName());
    });

    after(function () {
        assertDropCollection(testDB, testColl.getName());
    });

    beforeEach(function () {
        assert.commandWorked(testColl.insert([{_id: "present"}, {_id: "gone"}]));
    });

    afterEach(function () {
        assert.commandWorked(testColl.deleteMany({}));
    });

    it("records found / notFound into the engine's single-document-lookup cell", function () {
        const engine = expectedEngine(isRunningOptimizedUpdateLookup);

        const delta = ServerStatusMetrics.withServerStatusMetricsAcrossCluster(testDB, () => {
            withChangeStreamTest(testDB, (cst) => {
                const cursor = cst.startWatchingChanges({
                    pipeline: [{$changeStream: {fullDocument: "updateLookup"}}],
                    collection: testColl.getName(),
                });

                // 'present' still exists when the post-image is looked up -> recordFound.
                assert.commandWorked(testColl.update({_id: "present"}, {$set: {v: 1}}));

                // 'gone' is deleted before we drain the stream, so its update event's post-image
                // lookup finds nothing -> recordNotFound.
                assert.commandWorked(testColl.update({_id: "gone"}, {$set: {v: 1}}));
                assert.commandWorked(testColl.remove({_id: "gone"}));

                // Drain all 3 events (2 updates + 1 delete) so the server has completed both
                // post-image lookups before we read serverStatus.
                cst.getNextChanges(cursor, 3);
            });
        });

        const lookup = delta.changeStreams.updateLookup[engine];
        assert.eq(lookup.found, 1, {lookup});
        assert.eq(lookup.notFound, 1, {lookup});

        // Since there are no migration, the primary executor should always succeed.
        assert.eq(lookup.notHandled, 0, {lookup});

        // Two update events → exactly 2 post-image lookups total.
        assert.eq(lookup.found + lookup.notFound + lookup.notHandled, 2, {lookup});
    });
});
