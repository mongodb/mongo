/**
 * FSM test: Fetch-one-and-resume vs continuous reading mode comparison.
 * Verifies that both reading modes produce equivalent results.
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
import {runWithFsmCluster, verifyFetchAndResume} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FSM Fetch-One-And-Resume", function () {
    it("db absent", function () {
        runWithFsmCluster("foar_db_absent", (fsmSt, setupResult) => {
            verifyFetchAndResume(fsmSt, setupResult);
            jsTest.log.info(`✓ FETCH-ONE-AND-RESUME (db absent): verified via Verifier`);
        });
    });

    it("db present, no drops", function () {
        runWithFsmCluster(
            "foar_db_present_no_drops",
            (fsmSt, setupResult) => {
                verifyFetchAndResume(fsmSt, setupResult);
                jsTest.log.info(`✓ FETCH-ONE-AND-RESUME (db present, no drops): verified via Verifier`);
            },
            {startState: State.DATABASE_PRESENT_COLLECTION_ABSENT},
        );
    });
});
