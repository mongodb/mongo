/**
 * FSM test: Pairwise change stream comparison.
 * Runs two readers with different configurations on the same data and verifies
 * they produce identical event sequences.
 *
 * Modes (controlled by TestData):
 * - Default: V1 vs V2 change stream version comparison.
 * - pairwiseIrs: Strict vs ignoreRemovedShards comparison.
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
    verifyComparison,
    resolveWatchConfig,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it, afterEach} from "jstests/libs/mochalite.js";

describe("Change Stream Sharding FSM Comparison", function () {
    let env;
    afterEach(function () {
        env?.teardown();
    });

    it("db absent", function () {
        const {watchMode, writers} = resolveWatchConfig(State.DATABASE_ABSENT);
        env = setupFsmCluster("comparison_db_absent", {writers});
        verifyComparison(env, watchMode);
    });

    it("db present", function () {
        const {watchMode, writers} = resolveWatchConfig(State.DATABASE_PRESENT_COLLECTION_ABSENT);
        env = setupFsmCluster("comparison_db_present", {writers});
        verifyComparison(env, watchMode);
    });
});
