/**
 * FSM test: Fetch-one-and-resume vs continuous reading mode comparison.
 * Verifies that both reading modes produce equivalent results.
 *
 * @tags: [assumes_balancer_off, does_not_support_stepdowns, uses_change_streams]
 */
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {Verifier, DuplicateFilteringPairwiseTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {runWithFsmCluster} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("FSM Fetch-One-And-Resume", function () {
    it("compares reading modes", function () {
        const compareReadingModes = (fsmSt, {baseReaderConfig, createInstanceName}) => {
            const readerContinuous = createInstanceName("reader_continuous");
            const readerFoar = createInstanceName("reader_foar");
            const verifierInstanceName = createInstanceName("verifier");

            // Run Continuous reader first.
            const continuousConfig = {
                ...baseReaderConfig,
                instanceName: readerContinuous,
                readingMode: ChangeStreamReadingMode.kContinuous,
            };
            ChangeStreamReader.run(fsmSt.s, continuousConfig);

            // Get events and count duplicates (events with same clusterTime+operationType).
            Connector.waitForDone(fsmSt.s, readerContinuous);
            const continuousEvents = Connector.readAllChangeEvents(fsmSt.s, readerContinuous);

            const seenClusterTimeAndOp = new Map();
            for (const e of continuousEvents) {
                const ct = e.changeEvent.clusterTime;
                const key = `${ct.t},${ct.i},${e.changeEvent.operationType}`;
                seenClusterTimeAndOp.set(key, (seenClusterTimeAndOp.get(key) || 0) + 1);
            }
            let extraDuplicates = 0;
            for (const [, count] of seenClusterTimeAndOp) {
                if (count > 1) {
                    extraDuplicates += count - 1;
                }
            }
            const uniqueEventCount = continuousEvents.length - extraDuplicates;

            // Run FetchOneAndResume - reads unique events (duplicates skipped by resumeAfter).
            const foarConfig = {
                ...baseReaderConfig,
                instanceName: readerFoar,
                numberOfEventsToRead: uniqueEventCount,
                readingMode: ChangeStreamReadingMode.kFetchOneAndResume,
            };
            ChangeStreamReader.run(fsmSt.s, foarConfig);

            // DuplicateFilteringPairwiseTestCase filters duplicates before comparing.
            new Verifier().run(
                fsmSt.s,
                {
                    changeStreamReaderConfigs: {
                        [readerContinuous]: continuousConfig,
                        [readerFoar]: foarConfig,
                    },
                    matcherSpecsByInstance: {},
                    instanceName: verifierInstanceName,
                },
                [new DuplicateFilteringPairwiseTestCase(readerContinuous, readerFoar)],
            );

            jsTest.log.info(`✓ FETCH-ONE-AND-RESUME: verified via Verifier`);
        };

        runWithFsmCluster("foar", compareReadingModes);
    });
});
