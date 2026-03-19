/**
 * FSM test: Fetch-one-and-resume vs continuous reading mode comparison.
 * Verifies that both reading modes produce equivalent results.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, SequentialPairwiseFetchingTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

function verifyFetchAndResume(fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) {
    const readerContinuous = createInstanceName("reader_continuous");
    const readerFoar = createInstanceName("reader_foar");
    const verifierInstanceName = createInstanceName("verifier");

    const continuousConfig = {
        ...baseReaderConfig,
        instanceName: readerContinuous,
        readingMode: ChangeStreamReadingMode.kContinuous,
    };
    ChangeStreamReader.run(fsmSt.s, continuousConfig);

    const foarConfig = {
        ...baseReaderConfig,
        instanceName: readerFoar,
        readingMode: ChangeStreamReadingMode.kFetchOneAndResume,
    };
    ChangeStreamReader.run(fsmSt.s, foarConfig);

    const matcherSpecs = createMatcher(expectedEvents);

    new Verifier().run(
        fsmSt.s,
        {
            changeStreamReaderConfigs: {
                [readerContinuous]: continuousConfig,
                [readerFoar]: foarConfig,
            },
            matcherSpecsByInstance: {
                [readerContinuous]: matcherSpecs,
                [readerFoar]: matcherSpecs,
            },
            instanceName: verifierInstanceName,
        },
        [new SequentialPairwiseFetchingTestCase(readerContinuous, readerFoar)],
    );
}

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
