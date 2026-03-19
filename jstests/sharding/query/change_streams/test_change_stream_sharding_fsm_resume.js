/**
 * FSM test: Resume from cluster time verification.
 * Verifies that change streams can be resumed from various cluster times.
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
import {ChangeStreamReader} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, PrefixReadTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {createMatcher, runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {describe, it} from "jstests/libs/mochalite.js";

function verifyResume(fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) {
    const readerInstanceName = createInstanceName("reader");
    const verifierInstanceName = createInstanceName("verifier");

    const readerConfig = {...baseReaderConfig, instanceName: readerInstanceName};
    ChangeStreamReader.run(fsmSt.s, readerConfig);

    const shardConnections = [];
    for (let i = 0; fsmSt[`rs${i}`]; i++) {
        shardConnections.push(fsmSt[`rs${i}`].getPrimary());
    }

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
}

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
