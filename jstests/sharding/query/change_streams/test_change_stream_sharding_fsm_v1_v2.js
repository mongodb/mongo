/**
 * FSM test: V1 vs V2 change stream version comparison.
 * Verifies that both change stream versions produce identical events.
 *
 * @tags: [assumes_balancer_off, uses_change_streams]
 */
import {ChangeStreamReader} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, SequentialPairwiseFetchingTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

// TODO SERVER-117490: Re-enable once deferred matching is fully understood.
describe.skip("FSM V1 vs V2", function () {
    it("compares change stream versions", function () {
        runWithFsmCluster("v1_v2", (fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) => {
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

            jsTest.log.info(`✓ V1 vs V2: verified via Verifier`);
        });
    });
});
