/**
 * FSM test: Continuous reading mode verification.
 * Verifies that continuous reading mode captures all expected events.
 *
 * @tags: [assumes_balancer_off, uses_change_streams]
 */
import {ChangeStreamReader} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, SingleReaderVerificationTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

// TODO SERVER-117490: Re-enable once deferred matching is fully understood.
describe.skip("FSM Continuous", function () {
    it("verifies continuous reading mode", function () {
        runWithFsmCluster("continuous", (fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) => {
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

            jsTest.log.info(`✓ CONTINUOUS: verified via Verifier`);
        });
    });
});
