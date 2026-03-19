/**
 * FSM test: Continuous reading mode verification.
 * Verifies that continuous reading mode captures all expected events.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {ChangeStreamReader} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, SingleReaderVerificationTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

function verifyContinuous(fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) {
    const readerInstanceName = createInstanceName("reader");
    const verifierInstanceName = createInstanceName("verifier");

    const readerConfig = {...baseReaderConfig, instanceName: readerInstanceName};
    ChangeStreamReader.run(fsmSt.s, readerConfig);

    new Verifier().run(
        fsmSt.s,
        {
            changeStreamReaderConfigs: {[readerInstanceName]: readerConfig},
            matcherSpecsByInstance: {[readerInstanceName]: createMatcher(expectedEvents)},
            instanceName: verifierInstanceName,
        },
        [new SingleReaderVerificationTestCase(readerInstanceName)],
    );
}

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
