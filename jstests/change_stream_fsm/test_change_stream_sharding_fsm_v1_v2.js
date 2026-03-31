/**
 * FSM test: V1 vs V2 change stream version comparison.
 * Verifies that both change stream versions produce identical events.
 *
 * When run via a bg_mutator matrix suite variant, a concurrent BackgroundMutator
 * performs FCV flips and placement history resets alongside the Writer.
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
    runWithFsmCluster,
    verifyV1V2,
    TEST_DB,
    TEST_COLL,
    TEST_COLL_2,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FSM V1 vs V2", function () {
    it("db absent", function () {
        runWithFsmCluster(
            "v1_v2_db_absent",
            (fsmSt, setupResult) => {
                verifyV1V2(fsmSt, setupResult);
                jsTest.log.info(`✓ V1 vs V2 (db absent): verified via Verifier`);
            },
            {
                writers: [{dbName: TEST_DB, collName: TEST_COLL, startState: State.DATABASE_ABSENT}],
            },
        );
    });

    it("db present, no drops", function () {
        runWithFsmCluster(
            "v1_v2_db_present_no_drops",
            (fsmSt, setupResult) => {
                verifyV1V2(fsmSt, setupResult);
                jsTest.log.info(`✓ V1 vs V2 (db present, no drops): verified via Verifier`);
            },
            {
                writers: [
                    {dbName: TEST_DB, collName: TEST_COLL, startState: State.DATABASE_PRESENT_COLLECTION_ABSENT},
                    {dbName: TEST_DB, collName: TEST_COLL_2, startState: State.DATABASE_PRESENT_COLLECTION_ABSENT},
                ],
            },
        );
    });
});
