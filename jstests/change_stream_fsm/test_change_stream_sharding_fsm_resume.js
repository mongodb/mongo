/**
 * FSM test: Resume from cluster time verification.
 * Verifies that change streams can be resumed from various cluster times.
 *
 * The watch mode (collection, database, cluster) is controlled by TestData.watchMode,
 * set via matrix suite overrides for parallel execution across Evergreen tasks.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   # The test spins up a multi-shard cluster and runs DDL commands; too slow for
 *   # sanitizer builds that add significant overhead.
 *   incompatible_aubsan,
 *   requires_sharding,
 *   tsan_incompatible,
 *   uses_change_streams,
 * ]
 */
import {
    setupFsmCluster,
    verifyResume,
    resolveWatchConfig,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it, afterEach} from "jstests/libs/mochalite.js";

describe("Change Stream Sharding FSM Resume", function () {
    let env;
    afterEach(function () {
        env?.teardown();
    });

    it("db absent", function () {
        const startState = State.DATABASE_ABSENT;
        const {watchMode, writers} = resolveWatchConfig(startState);
        env = setupFsmCluster("resume_db_absent", {writers});
        verifyResume(env, watchMode, startState);
    });

    it("db present", function () {
        const startState = State.DATABASE_PRESENT_COLLECTION_ABSENT;
        const {watchMode, writers} = resolveWatchConfig(startState);
        env = setupFsmCluster("resume_db_present", {writers});
        verifyResume(env, watchMode, startState);
    });
});
