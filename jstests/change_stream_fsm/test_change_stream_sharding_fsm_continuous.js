/**
 * FSM test: Continuous reading mode verification.
 * Verifies that continuous reading mode captures all expected events.
 *
 * When run via a bg_mutator matrix suite variant, a concurrent BackgroundMutator
 * performs FCV flips and placement history resets alongside the Writer.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {runWithFsmCluster, verifyContinuous} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FSM Continuous", function () {
    it("db absent", function () {
        runWithFsmCluster("continuous_db_absent", (fsmSt, setupResult) => {
            verifyContinuous(fsmSt, setupResult);
            jsTest.log.info(`✓ CONTINUOUS (db absent): verified via Verifier`);
        });
    });

    it("db present, no drops", function () {
        runWithFsmCluster(
            "continuous_db_present_no_drops",
            (fsmSt, setupResult) => {
                verifyContinuous(fsmSt, setupResult);
                jsTest.log.info(`✓ CONTINUOUS (db present, no drops): verified via Verifier`);
            },
            {startState: State.DATABASE_PRESENT_COLLECTION_ABSENT},
        );
    });
});
