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
import {
    runWithFsmCluster,
    verifyFetchAndResume,
    TEST_DB,
    TEST_COLL,
    TEST_COLL_2,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FSM Fetch-One-And-Resume", function () {
    it("db absent", function () {
        runWithFsmCluster(
            "foar_db_absent",
            (fsmSt, setupResult) => {
                verifyFetchAndResume(fsmSt, setupResult);
                jsTest.log.info(`✓ FETCH-AND-RESUME (db absent): verified via Verifier`);
            },
            {
                writers: [{dbName: TEST_DB, collName: TEST_COLL, startState: State.DATABASE_ABSENT}],
            },
        );
    });

    it("db present, no drops", function () {
        runWithFsmCluster(
            "foar_db_present_no_drops",
            (fsmSt, setupResult) => {
                verifyFetchAndResume(fsmSt, setupResult);
                jsTest.log.info(`✓ FETCH-AND-RESUME (db present, no drops): verified via Verifier`);
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
