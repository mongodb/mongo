/**
 * FSM test: Resume from cluster time verification.
 * Verifies that change streams can be resumed from various cluster times.
 *
 * When run via a bg_mutator matrix suite variant, a concurrent BackgroundMutator
 * performs FCV flips and placement history resets alongside the Writer.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   # The test spins up a multi-shard cluster and runs DDL commands; too slow for
 *   # sanitizer builds that add significant overhead.
 *   incompatible_aubsan,
 *   requires_sharding,
 *   tsan_incompatible,
 *   uses_change_streams,
 * ]
 */
import {runWithFsmCluster, verifyResume} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FSM Resume", function () {
    it("db absent", function () {
        runWithFsmCluster("resume_db_absent", (fsmSt, setupResult) => {
            verifyResume(fsmSt, setupResult);
            jsTest.log.info(`✓ RESUME (db absent): verified via Verifier`);
        });
    });

    it("db present, no drops", function () {
        runWithFsmCluster(
            "resume_db_present_no_drops",
            (fsmSt, setupResult) => {
                verifyResume(fsmSt, setupResult);
                jsTest.log.info(`✓ RESUME (db present, no drops): verified via Verifier`);
            },
            {startState: State.DATABASE_PRESENT_COLLECTION_ABSENT},
        );
    });
});
