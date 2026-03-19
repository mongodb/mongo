/**
 * FSM test: V1 vs V2 change stream version comparison.
 * Verifies that both change stream versions produce identical events.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {ChangeStreamReader} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, SequentialPairwiseFetchingTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

function verifyV1V2(fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) {
    const readerV1 = createInstanceName("reader_v1");
    const readerV2 = createInstanceName("reader_v2");
    const verifierInstanceName = createInstanceName("verifier");

    const readerConfigs = {
        [readerV1]: {...baseReaderConfig, instanceName: readerV1, version: "v1"},
        [readerV2]: {...baseReaderConfig, instanceName: readerV2, version: "v2"},
    };

    ChangeStreamReader.run(fsmSt.s, readerConfigs[readerV1]);
    ChangeStreamReader.run(fsmSt.s, readerConfigs[readerV2]);

    new Verifier().run(
        fsmSt.s,
        {
            changeStreamReaderConfigs: readerConfigs,
            matcherSpecsByInstance: {
                [readerV1]: createMatcher(expectedEvents),
                [readerV2]: createMatcher(expectedEvents),
            },
            instanceName: verifierInstanceName,
        },
        [new SequentialPairwiseFetchingTestCase(readerV1, readerV2)],
    );
}

describe("FSM V1 vs V2", function () {
    it("db absent", function () {
        runWithFsmCluster("v1_v2_db_absent", (fsmSt, setupResult) => {
            verifyV1V2(fsmSt, setupResult);
            jsTest.log.info(`✓ V1 vs V2 (db absent): verified via Verifier`);
        });
    });

    it("db present, no drops", function () {
        runWithFsmCluster(
            "v1_v2_db_present_no_drops",
            (fsmSt, setupResult) => {
                verifyV1V2(fsmSt, setupResult);
                jsTest.log.info(`✓ V1 vs V2 (db present, no drops): verified via Verifier`);
            },
            {startState: State.DATABASE_PRESENT_COLLECTION_ABSENT},
        );
    });
});
