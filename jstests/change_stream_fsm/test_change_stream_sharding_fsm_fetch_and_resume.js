/**
 * FSM test: Fetch-one-and-resume vs continuous reading mode comparison.
 * Verifies that both reading modes produce equivalent results.
 *
 * The watch mode (collection, database, cluster) is controlled by TestData.watchMode,
 * set via matrix suite overrides for parallel execution across Evergreen tasks.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {
    setupFsmCluster,
    verifyFetchAndResume,
    resolveWatchConfig,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it, afterEach} from "jstests/libs/mochalite.js";

describe("Change Stream Sharding FSM Fetch-One-And-Resume", function () {
    let env;
    afterEach(function () {
        env?.teardown();
    });

    it("db absent", function () {
        const {watchMode, writers} = resolveWatchConfig(State.DATABASE_ABSENT);
        env = setupFsmCluster("foar_db_absent", {writers});
        verifyFetchAndResume(env, watchMode);
    });

    it("db present", function () {
        const {watchMode, writers} = resolveWatchConfig(State.DATABASE_PRESENT_COLLECTION_ABSENT);
        env = setupFsmCluster("foar_db_present", {writers});
        verifyFetchAndResume(env, watchMode);
    });
});
