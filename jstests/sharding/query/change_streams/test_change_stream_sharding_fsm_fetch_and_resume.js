/**
 * FSM test: Fetch-one-and-resume vs continuous reading mode comparison.
 * Verifies that both reading modes produce equivalent results.
 *
 * @tags: [assumes_balancer_off, does_not_support_stepdowns, uses_change_streams]
 */
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {ShardingCommandGenerator} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator.js";
import {ShardingCommandGeneratorParams} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator_params.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ChangeEventMatcher} from "jstests/libs/util/change_stream/change_stream_event.js";
import {SingleChangeStreamMatcher} from "jstests/libs/util/change_stream/change_stream_matcher.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {Verifier, DuplicateFilteringPairwiseTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {describe, it} from "jstests/libs/mochalite.js";

const TEST_DB = "test_cs";
const TEST_SEED = 42;

function getCurrentClusterTime(conn, dbName) {
    const result = conn.adminCommand({
        appendOplogNote: 1,
        data: {msg: "getCurrentClusterTime barrier"},
    });
    assert.commandWorked(result);
    return result.$clusterTime.clusterTime;
}

function createShardingTest(mongos = 1, shards = 2, rsNodes = 1) {
    return new ShardingTest({
        shards: shards,
        mongos: mongos,
        config: 1,
        rs: {
            nodes: rsNodes,
            setParameter: {
                writePeriodicNoops: true,
                periodicNoopIntervalSecs: 1,
            },
        },
        other: {
            enableBalancer: false,
        },
    });
}

function setupFsmTest(ctx, testName) {
    Random.setRandomSeed(TEST_SEED);
    const dbName = TEST_DB;
    const collName = "test_coll_fsm";
    const ts = Date.now();

    const params = new ShardingCommandGeneratorParams(dbName, collName, ctx.fsmShards);
    const generator = new ShardingCommandGenerator(TEST_SEED);
    const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
    const commands = generator.generateCommands(testModel, params);

    const expectedEvents = commands
        .flatMap((cmd) => cmd.getChangeEvents(ChangeStreamWatchMode.kCollection))
        .map((e) => ({event: e, cursorClosed: e.operationType === "invalidate"}));

    jsTest.log.info(`FSM ${testName}: Cluster config - shards: ${ctx.fsmShards.length}, mongos: 1`);
    jsTest.log.info(
        `FSM ${testName}: Generated ${commands.length} commands, expecting ${expectedEvents.length} events`,
    );
    jsTest.log.info(`FSM ${testName}: Expected events: ${tojson(expectedEvents.map((e) => e.event.operationType))}`);

    const startTime = getCurrentClusterTime(ctx.fsmSt.s, dbName);

    const writerInstanceName = `writer_${testName}_${ts}`;
    ctx.fsmInstancesToCleanup.push(writerInstanceName);
    Writer.run(ctx.fsmSt.s, {commands, instanceName: writerInstanceName});

    jsTest.log.info(`FSM ${testName}: Commands executed, starting change stream at ${tojson(startTime)}`);

    const createInstanceName = (prefix) => {
        const name = `${prefix}_${testName}_${ts}`;
        ctx.fsmInstancesToCleanup.push(name);
        return name;
    };

    const baseReaderConfig = {
        watchMode: ChangeStreamWatchMode.kCollection,
        dbName,
        collName,
        readingMode: ChangeStreamReadingMode.kContinuous,
        startAtClusterTime: startTime,
        numberOfEventsToRead: expectedEvents.length,
    };

    return {dbName, collName, commands, expectedEvents, baseReaderConfig, createInstanceName};
}

function runWithFsmCluster(testName, testFn, mongos = 1, shards = 1, rsNodes = 1) {
    const fsmSt = createShardingTest(mongos, shards, rsNodes);
    const fsmShards = assert.commandWorked(fsmSt.s.adminCommand({listShards: 1})).shards;
    const instancesToCleanup = [];

    try {
        const ctx = {fsmSt, fsmShards, fsmInstancesToCleanup: instancesToCleanup};
        const setupResult = setupFsmTest(ctx, testName);
        testFn(fsmSt, setupResult, instancesToCleanup);
    } finally {
        for (const instanceName of instancesToCleanup) {
            Connector.cleanup(fsmSt.s, instanceName);
        }
        fsmSt.s.getDB(TEST_DB).dropDatabase();
        fsmSt.stop();
    }
}

describe("FSM Fetch-One-And-Resume", function () {
    it("compares reading modes", function () {
        const compareReadingModes = (fsmSt, {expectedEvents, baseReaderConfig, createInstanceName}) => {
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
