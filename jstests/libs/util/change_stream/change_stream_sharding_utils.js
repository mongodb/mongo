/**
 * Shared test helpers for change stream FSM tests.
 * Provides common infrastructure for setting up sharding tests, managing cluster time,
 * and running FSM-based change stream verification tests.
 */
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {ShardingCommandGenerator} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator.js";
import {ShardingCommandGeneratorParams} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator_params.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {CreateDatabaseCommand} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ChangeEventMatcher} from "jstests/libs/util/change_stream/change_stream_event.js";
import {SingleChangeStreamMatcher} from "jstests/libs/util/change_stream/change_stream_matcher.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {
    Verifier,
    SingleReaderVerificationTestCase,
    PrefixReadTestCase,
    SequentialPairwiseFetchingTestCase,
} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {
    BackgroundMutator,
    BackgroundMutatorOpType,
} from "jstests/libs/util/change_stream/change_stream_background_mutator.js";

// Default test database and collection names.
const TEST_DB = "test_cs";
const TEST_DB_2 = "test_cs_2";
const TEST_COLL = "test_coll_fsm";
const TEST_COLL_2 = "test_coll_fsm_2";

// Random seed for the entire test run, logged for reproducibility.
const TEST_SEED = Date.now();

/**
 * Operation types to filter out before comparison.
 * These events have unpredictable behavior in multi-shard clusters:
 * - createIndexes: Emitted per-shard, count depends on shard distribution
 * - dropIndexes: Emitted per-shard, count depends on shard distribution
 * - modify: Emitted when collection metadata changes during FCV transitions
 *   triggered by the BackgroundMutator (flipFCV op).
 */
const kExcludedOperationTypes = ["createIndexes", "dropIndexes", "modify"];

/**
 * Get a fresh cluster time by doing a no-op write.
 * This ensures we get a timestamp that corresponds to an actual oplog entry,
 * which is required for reliable change stream resume tokens.
 * @param {Mongo} conn - MongoDB connection
 * @param {string} [dbName] - Database name (unused, kept for API compatibility)
 * @returns {Timestamp} The cluster time
 */
function getCurrentClusterTime(conn, dbName) {
    const result = conn.adminCommand({
        appendOplogNote: 1,
        data: {msg: "getCurrentClusterTime barrier"},
    });
    assert.commandWorked(result);
    return result.$clusterTime.clusterTime;
}

/**
 * Create a ShardingTest with standard configuration for change stream tests.
 * - Periodic no-ops enabled for oplog freshness
 * - Balancer disabled for test stability
 * @param {number} [mongos=1] - Number of mongos instances
 * @param {number} [shards=3] - Number of shards (3 enables moveChunk across shards)
 * @param {number} [rsNodes=1] - Number of replica set nodes per shard
 * @param {boolean} [configShard=false] - If true, one shard doubles as the config server
 * @returns {ShardingTest} The configured sharding test
 */
function createShardingTest(mongos = 1, shards = 3, rsNodes = 1, configShard = false) {
    const stOptions = {
        shards: shards,
        mongos: mongos,
        configShard: configShard,
        rs: {
            nodes: rsNodes,
            setParameter: {
                writePeriodicNoops: true,
                periodicNoopIntervalSecs: 1,
            },
        },
        other: {
            enableBalancer: false,
            configOptions: {
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                },
            },
        },
    };
    // With a dedicated config server (non-configShard), use a single-member RS
    // instead of the default 3: these tests don't exercise config server failover.
    if (!configShard) {
        stOptions.config = 1;
    }
    return new ShardingTest(stOptions);
}

/**
 * Create a SingleChangeStreamMatcher from expected events.
 * @param {Array<{event: Object, cursorClosed: boolean}>} expectedEvents - Expected events with metadata
 * @returns {SingleChangeStreamMatcher} The configured matcher
 */
function createMatcher(expectedEvents) {
    return new SingleChangeStreamMatcher(expectedEvents.map((e) => new ChangeEventMatcher(e.event, e.cursorClosed)));
}

/**
 * Compute expected change stream events from a command sequence for a given watch mode.
 */
function computeExpectedEvents(commands, watchMode) {
    return commands
        .flatMap((cmd) => cmd.getChangeEvents(watchMode))
        .map((e) => ({event: e, cursorClosed: e.operationType === "invalidate"}));
}

function buildCommandTrace(commands) {
    return commands.map((cmd, i) => {
        const shardIds = Array.isArray(cmd.shardSet) ? cmd.shardSet.map((s) => s._id) : [];
        return {
            cmdIndex: i,
            cmdName: cmd.constructor.name,
            events: cmd.getChangeEvents(ChangeStreamWatchMode.kCollection).map((e) => e.operationType),
            ctx: cmd.collectionCtx ?? null,
            shardSet: shardIds,
            primaryShard: cmd.primaryShard ? cmd.primaryShard._id : null,
            targetShardKey: cmd.targetShardKey ?? null,
        };
    });
}

/**
 * Build reader specs for each writer's collection.
 * Each spec bundles expected events, reader config, label, and a createMatcher() factory.
 *
 * @param {Array<{dbName: string, collName: string, commands: Array}>} commandsByWriter
 * @param {Timestamp} startTime - Cluster time to start reading from.
 * @param {number} [batchSize] - Optional cursor batch size (undefined = server default).
 * @returns {Array<Object>} Reader specs with label, expectedEvents, createMatcher, config.
 */
function buildReaderSpecs(commandsByWriter, startTime, batchSize) {
    const specs = [];

    for (const w of commandsByWriter) {
        const expectedEvents = computeExpectedEvents(w.commands, ChangeStreamWatchMode.kCollection);
        const commandTrace = buildCommandTrace(w.commands);
        specs.push({
            label: `coll_${w.dbName}_${w.collName}`,
            expectedEvents,
            createMatcher: () => createMatcher(expectedEvents),
            config: {
                watchMode: ChangeStreamWatchMode.kCollection,
                dbName: w.dbName,
                collName: w.collName,
                readingMode: ChangeStreamReadingMode.kContinuous,
                startAtClusterTime: startTime,
                numberOfEventsToRead: expectedEvents.length,
                excludeOperationTypes: kExcludedOperationTypes,
                batchSize,
                debugCommandTrace: commandTrace,
            },
        });
    }

    return specs;
}

/**
 * Pre-create databases for writers that start from a non-absent state.
 */
function ensureDatabasesExist(writerDefs, mongos, fsmShards) {
    const createdDbs = new Set();
    for (const w of writerDefs) {
        const startState = w.startState || State.DATABASE_PRESENT_COLLECTION_ABSENT;
        if (startState !== State.DATABASE_ABSENT && !createdDbs.has(w.dbName)) {
            createdDbs.add(w.dbName);
            mongos.getDB(w.dbName).dropDatabase();
            new CreateDatabaseCommand(w.dbName, w.collName, fsmShards).execute(mongos);
        }
    }
}

/**
 * Generate FSM commands for each writer definition.
 */
function generateCommandsForWriters(writerDefs, fsmShards) {
    const commandsByWriter = [];
    for (const w of writerDefs) {
        const startState = w.startState || State.DATABASE_PRESENT_COLLECTION_ABSENT;
        const params = new ShardingCommandGeneratorParams(w.dbName, w.collName, fsmShards);
        const generator = new ShardingCommandGenerator(TEST_SEED);
        const testModel = new CollectionTestModel(startState);
        const commands = generator.generateCommands(testModel, params);
        commandsByWriter.push({dbName: w.dbName, collName: w.collName, commands});
    }
    return commandsByWriter;
}

/**
 * Launch a Writer for each generated command sequence.
 */
function runWriters(ctx, testName, ts, commandsByWriter) {
    const writerInstanceNames = [];
    for (const w of commandsByWriter) {
        const writerInstanceName = `writer_${testName}_${w.dbName}_${w.collName}_${ts}`;
        ctx.fsmInstancesToCleanup.push(writerInstanceName);
        writerInstanceNames.push(writerInstanceName);
        Writer.run(ctx.fsmSt.s, writerInstanceName, w.commands, TEST_SEED);
    }
    return writerInstanceNames;
}

/**
 * Start a BackgroundMutator that runs concurrent cluster operations
 * (e.g. FCV flips, resetPlacementHistory) until all writers complete.
 */
function startBackgroundMutator(ctx, testName, ts, bgMutatorOpts, writerInstanceNames) {
    const mutatorInstanceName = `bgmutator_${testName}_${ts}`;
    ctx.fsmInstancesToCleanup.push(mutatorInstanceName);
    BackgroundMutator.start(ctx.fsmSt.s, {
        ops: [BackgroundMutatorOpType.ResetPlacementHistory, BackgroundMutatorOpType.FlipFCV],
        shardingTest: ctx.fsmSt,
        seed: TEST_SEED,
        ...bgMutatorOpts,
        instanceName: mutatorInstanceName,
        stopInstanceNames: writerInstanceNames,
    });
}

/**
 * Set up an FSM test with one or more parallel writers and reader specs.
 *
 * @param {Object} ctx - Test context with fsmSt, fsmShards, fsmInstancesToCleanup
 * @param {string} testName - Unique test name for logging and instance naming
 * @param {Object} [options] - Optional configuration
 * @param {Array<{dbName: string, collName: string, startState: number}>} options.writers -
 *   Writer definitions.  Each entry specifies the database, collection, and FSM start state.
 *   Writers sharing a database should use DB_PRESENT_COLLECTION_ABSENT to avoid
 *   one writer's DROP_DATABASE destroying another's collections.
 * @returns {Object} { readerSpecs, createInstanceName }
 */
function setupFsmTest(ctx, testName, options = {}) {
    jsTest.log.info(`FSM ${testName}: using seed ${TEST_SEED}`);
    Random.setRandomSeed(TEST_SEED);
    const ts = Date.now();

    const writerDefs = options.writers;

    ensureDatabasesExist(writerDefs, ctx.fsmSt.s, ctx.fsmShards);

    const commandsByWriter = generateCommandsForWriters(writerDefs, ctx.fsmShards);
    const startTime = getCurrentClusterTime(ctx.fsmSt.s);
    const writerInstanceNames = runWriters(ctx, testName, ts, commandsByWriter);

    if (options.bgMutator) {
        startBackgroundMutator(ctx, testName, ts, options.bgMutator, writerInstanceNames);
    }

    const batchSize = TestData.batchSize || undefined;
    const readerSpecs = buildReaderSpecs(commandsByWriter, startTime, batchSize);

    jsTest.log.info("FSM setup complete", {
        testName,
        shards: ctx.fsmShards.length,
        writers: writerDefs.length,
        readerLabels: readerSpecs.map((r) => `${r.label}(${r.expectedEvents.length})`),
    });

    const createInstanceName = (prefix) => {
        const name = `${prefix}_${testName}_${ts}`;
        ctx.fsmInstancesToCleanup.push(name);
        return name;
    };

    return {readerSpecs, createInstanceName};
}

/**
 * Run each teardown step independently so one failure doesn't prevent the rest.
 * Collects all errors and throws a combined error at the end.
 */
function runTeardownSteps(...steps) {
    const errors = [];
    for (const step of steps) {
        try {
            step();
        } catch (e) {
            errors.push(e);
        }
    }
    if (errors.length > 0) {
        throw new Error(
            `Teardown encountered ${errors.length} error(s):\n` + errors.map((e) => e.toString()).join("\n"),
        );
    }
}

/**
 * Run an FSM test with automatic cluster setup and teardown.
 * Creates a ShardingTest, sets up the FSM test, runs the test function, and cleans up.
 * @param {string} testName - Unique test name
 * @param {Function} testFn - Test function receiving (fsmSt, setupResult)
 * @param {Object} [options] - Optional configuration
 * @param {number} [options.mongos=1] - Number of mongos instances
 * @param {number} [options.shards=3] - Number of shards
 * @param {number} [options.rsNodes=1] - Number of replica set nodes per shard
 * @param {Array} [options.writers] - Writer definitions (passed to setupFsmTest)
 */
function runWithFsmCluster(testName, testFn, options = {}) {
    const {mongos = 1, shards = 3, rsNodes = 1} = options;
    const configShard = TestData.configShard || false;
    const fsmSt = createShardingTest(mongos, shards, rsNodes, configShard);
    const fsmShards = assert.commandWorked(fsmSt.s.adminCommand({listShards: 1})).shards;
    const instancesToCleanup = [];

    try {
        const ctx = {fsmSt, fsmShards, fsmInstancesToCleanup: instancesToCleanup};
        const setupResult = setupFsmTest(ctx, testName, options);
        testFn(fsmSt, setupResult);
    } finally {
        runTeardownSteps(
            () => Writer.joinAll(),
            () => BackgroundMutator.join(),
            () => ChangeStreamReader.joinAll(),
            ...instancesToCleanup.map((name) => () => Connector.cleanup(fsmSt.s, name)),
            () => fsmSt.stop(),
        );
    }
}

/**
 * Generic scope-level verification: spin up readers, build matchers, and run a Verifier.
 *
 * @param {ShardingTest} fsmSt - The sharding test fixture.
 * @param {Object} spec - Reader spec from buildReaderSpecs (label, config, createMatcher).
 * @param {Function} createInstanceName - Factory returned by setupFsmTest.
 * @param {Object} opts
 * @param {Array<{suffix: string, configOverrides: Object}>} opts.readers -
 *   One entry per reader to create.  Each gets an instance name derived from
 *   `spec.label` and the suffix, and a config that merges spec.config with
 *   the overrides.
 * @param {Function} opts.createTestCases - (readerNamesBySuffix) => TestCase[].
 * @param {Object} [opts.extraVerifierConfig] - Extra fields merged into the
 *   Verifier config (e.g. shardConnections).
 */
function verifyScope(fsmSt, spec, createInstanceName, {readers, createTestCases, extraVerifierConfig = {}}) {
    const readerNamesBySuffix = {};
    const readerConfigs = {};
    const matcherSpecsByInstance = {};

    for (const {suffix, configOverrides} of readers) {
        const name = createInstanceName(`reader_${suffix}_${spec.label}`);
        const config = {...spec.config, ...configOverrides, instanceName: name};
        readerNamesBySuffix[suffix] = name;
        readerConfigs[name] = config;
        ChangeStreamReader.run(fsmSt.s, config);
        matcherSpecsByInstance[name] = spec.createMatcher();
    }

    new Verifier().run(
        fsmSt.s,
        {
            changeStreamReaderConfigs: readerConfigs,
            matcherSpecsByInstance,
            instanceName: createInstanceName(`verifier_${spec.label}`),
            ...extraVerifierConfig,
        },
        createTestCases(readerNamesBySuffix),
    );
}

function verifyContinuous(fsmSt, {readerSpecs, createInstanceName}) {
    for (const spec of readerSpecs) {
        verifyScope(fsmSt, spec, createInstanceName, {
            readers: [{suffix: "cont", configOverrides: {}}],
            createTestCases: (m) => [new SingleReaderVerificationTestCase(m.cont)],
        });
    }
}

function verifyResume(fsmSt, {readerSpecs, createInstanceName}) {
    const shardConnections = [];
    for (let i = 0; fsmSt[`rs${i}`]; i++) {
        shardConnections.push(fsmSt[`rs${i}`].getPrimary());
    }
    for (const spec of readerSpecs) {
        verifyScope(fsmSt, spec, createInstanceName, {
            readers: [{suffix: "resume", configOverrides: {}}],
            createTestCases: (m) => [new PrefixReadTestCase(m.resume, 3)],
            extraVerifierConfig: {shardConnections},
        });
    }
}

function verifyV1V2(fsmSt, {readerSpecs, createInstanceName}) {
    for (const spec of readerSpecs) {
        verifyScope(fsmSt, spec, createInstanceName, {
            readers: [
                {suffix: "v1", configOverrides: {version: "v1"}},
                {suffix: "v2", configOverrides: {version: "v2"}},
            ],
            createTestCases: (m) => [new SequentialPairwiseFetchingTestCase(m.v1, m.v2)],
        });
    }
}

function verifyFetchAndResume(fsmSt, {readerSpecs, createInstanceName}) {
    for (const spec of readerSpecs) {
        verifyScope(fsmSt, spec, createInstanceName, {
            readers: [
                {suffix: "cont", configOverrides: {readingMode: ChangeStreamReadingMode.kContinuous}},
                {suffix: "foar", configOverrides: {readingMode: ChangeStreamReadingMode.kFetchOneAndResume}},
            ],
            createTestCases: (m) => [new SequentialPairwiseFetchingTestCase(m.cont, m.foar)],
        });
    }
}

export {
    TEST_DB,
    TEST_DB_2,
    TEST_COLL,
    TEST_COLL_2,
    TEST_SEED,
    kExcludedOperationTypes,
    getCurrentClusterTime,
    createShardingTest,
    createMatcher,
    computeExpectedEvents,
    setupFsmTest,
    runWithFsmCluster,
    BackgroundMutatorOpType,
    verifyScope,
    verifyContinuous,
    verifyResume,
    verifyV1V2,
    verifyFetchAndResume,
};
