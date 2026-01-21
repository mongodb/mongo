/**
 * FSM test: Resume from cluster time verification.
 * Verifies that change streams can be resumed from various cluster times.
 *
 * @tags: [assumes_balancer_off, uses_change_streams]
 */
import {ChangeStreamReader} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, PrefixReadTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

// TODO SERVER-117490: Re-enable once deferred matching is fully understood.
describe.skip("FSM Resume", function () {
    it("verifies resume from cluster time", function () {
        runWithFsmCluster("resume", (fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) => {
            const readerInstanceName = createInstanceName("reader");
            const verifierInstanceName = createInstanceName("verifier");

            const readerConfig = {...baseReaderConfig, instanceName: readerInstanceName};
            ChangeStreamReader.run(fsmSt.s, readerConfig);

            const shardConnections = [fsmSt.rs0.getPrimary()];

            new Verifier().run(
                fsmSt.s,
                {
                    changeStreamReaderConfigs: {[readerInstanceName]: readerConfig},
                    matcherSpecsByInstance: {[readerInstanceName]: createMatcher(expectedEvents)},
                    instanceName: verifierInstanceName,
                    shardConnections,
                },
                [new PrefixReadTestCase(readerInstanceName, 3)],
            );

            jsTest.log.info(`✓ RESUME: verified via Verifier`);
        });
    });
});
