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
import {
    SingleChangeStreamMatcher,
    MultipleChangeStreamMatcher,
} from "jstests/libs/util/change_stream/change_stream_matcher.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {
    Verifier,
    SingleReaderVerificationTestCase,
    PrefixReadTestCase,
    SequentialPairwiseFetchingTestCase,
} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {ChangeStreamWatchMode, getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {
    BackgroundMutator,
    BackgroundMutatorOpType,
} from "jstests/libs/util/change_stream/change_stream_background_mutator.js";
import {removeShard, moveOutSessionChunks} from "jstests/sharding/libs/remove_shard_util.js";

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
 * Get the current cluster time without writing to the oplog.
 * @param {Mongo} conn - MongoDB connection
 * @param {string} [dbName] - Database name (unused, kept for API compatibility)
 * @returns {Timestamp} The cluster time
 */
function getCurrentClusterTime(conn, dbName) {
    return getClusterTime(conn.getDB("admin"));
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
 * Create a MultipleChangeStreamMatcher from per-collection expected event lists.
 * Each sub-list is matched independently; events from different collections can interleave.
 * @param {Array<Array<{event: Object, cursorClosed: boolean}>>} perCollectionEvents
 * @returns {MultipleChangeStreamMatcher}
 */
function createCompositeMatcher(perCollectionEvents) {
    const subMatchers = perCollectionEvents.map(
        (events) => new SingleChangeStreamMatcher(events.map((e) => new ChangeEventMatcher(e.event, e.cursorClosed))),
    );
    return new MultipleChangeStreamMatcher(subMatchers);
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
 * Build reader specs for the given watch mode.
 *
 * Returns one spec per scope entity:
 * - kCollection: one spec per writer (each watches its own namespace).
 * - kDb: one spec per database (aggregating writers that share a db).
 * - kCluster: always a single spec aggregating all writers.
 *
 * For collection-level: createMatcher() returns a SingleChangeStreamMatcher.
 * For db/cluster-level: createMatcher() returns a MultipleChangeStreamMatcher with one
 *   sub-matcher per collection, since events from different collections can interleave.
 *
 * @param {Array<{dbName: string, collName: string, commands: Array}>} commandsByWriter
 * @param {Timestamp} startTime - Cluster time to start reading from.
 * @param {number} [batchSize] - Optional cursor batch size (undefined = server default).
 * @param {number} watchMode - ChangeStreamWatchMode (kCollection, kDb, kCluster).
 * @returns {Array<Object>} Reader specs with label, createMatcher, config.
 */
function buildReaderSpecs(commandsByWriter, startTime, batchSize, watchMode) {
    const baseConfig = {
        readingMode: ChangeStreamReadingMode.kContinuous,
        startAtClusterTime: startTime,
        excludeOperationTypes: kExcludedOperationTypes,
        batchSize,
    };

    switch (watchMode) {
        case ChangeStreamWatchMode.kCollection:
            return commandsByWriter.map((w) => {
                const events = computeExpectedEvents(w.commands, watchMode);
                return {
                    label: `coll_${w.dbName}_${w.collName}`,
                    createMatcher: () => createMatcher(events),
                    config: {
                        ...baseConfig,
                        watchMode,
                        dbName: w.dbName,
                        collName: w.collName,
                        numberOfEventsToRead: events.length,
                        debugCommandTrace: buildCommandTrace(w.commands),
                    },
                };
            });

        case ChangeStreamWatchMode.kDb: {
            const writersByDb = {};
            for (const w of commandsByWriter) {
                (writersByDb[w.dbName] ??= []).push(w);
            }
            return Object.entries(writersByDb).map(([dbName, writers]) => {
                const perCollEvents = writers.map((w) => computeExpectedEvents(w.commands, watchMode));
                const eventCount = perCollEvents.reduce((s, g) => s + g.length, 0);
                return {
                    label: `db_${dbName}`,
                    createMatcher: () => createCompositeMatcher(perCollEvents),
                    config: {
                        ...baseConfig,
                        watchMode,
                        dbName,
                        collName: writers[0].collName,
                        numberOfEventsToRead: eventCount,
                    },
                };
            });
        }

        case ChangeStreamWatchMode.kCluster: {
            const perCollEvents = commandsByWriter.map((w) => computeExpectedEvents(w.commands, watchMode));
            const eventCount = perCollEvents.reduce((s, g) => s + g.length, 0);
            return [
                {
                    label: "cluster",
                    createMatcher: () => createCompositeMatcher(perCollEvents),
                    config: {
                        ...baseConfig,
                        watchMode,
                        dbName: "admin",
                        collName: null,
                        numberOfEventsToRead: eventCount,
                    },
                },
            ];
        }

        default:
            throw new Error(`Unknown watchMode: ${watchMode}`);
    }
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
 * Set up an FSM test: run writers (and optional background mutator), capture the
 * cluster time boundary. Mode-agnostic -- reader specs are built later at verify time.
 *
 * @param {Object} ctx - Test context with fsmSt, fsmShards, fsmInstancesToCleanup
 * @param {string} testName - Unique test name for logging and instance naming
 * @param {Object} [options] - Optional configuration
 * @param {Array<{dbName: string, collName: string, startState: number}>} options.writers -
 *   Writer definitions.  Each entry specifies the database, collection, and FSM start state.
 *   Writers sharing a database should use DB_PRESENT_COLLECTION_ABSENT to avoid
 *   one writer's DROP_DATABASE destroying another's collections.
 * @returns {Object} { commandsByWriter, startTime, createInstanceName }
 */
function setupFsmTest(ctx, testName, options = {}) {
    jsTest.log.info(`FSM ${testName}: using seed ${TEST_SEED}`);
    Random.setRandomSeed(TEST_SEED);
    const ts = Date.now();

    const writerDefs = options.writers;

    ensureDatabasesExist(writerDefs, ctx.fsmSt.s, ctx.fsmShards);

    const commandsByWriter = generateCommandsForWriters(writerDefs, ctx.fsmShards);
    const startTime = getClusterTime(ctx.fsmSt.s.getDB("admin"));
    const writerInstanceNames = runWriters(ctx, testName, ts, commandsByWriter);

    if (options.bgMutator) {
        startBackgroundMutator(ctx, testName, ts, options.bgMutator, writerInstanceNames);
    }

    jsTest.log.info("FSM setup complete", {
        testName,
        shards: ctx.fsmShards.length,
        writers: writerDefs.length,
    });

    const createInstanceName = (prefix) => {
        const name = `${prefix}_${testName}_${ts}`;
        ctx.fsmInstancesToCleanup.push(name);
        return name;
    };

    return {commandsByWriter, startTime, createInstanceName};
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
 * Set up an FSM cluster and return everything needed for test hooks.
 * Mode-agnostic: runs writers and captures cluster state. Reader specs are
 * built later by the verify functions when watchMode is known.
 * @param {string} testName - Unique test name for instance naming.
 * @param {Object} [options] - Passed to setupFsmTest (writers, bgMutator, etc.).
 * @returns {Object} { fsmSt, commandsByWriter, startTime, createInstanceName, teardown() }
 */
function setupFsmCluster(testName, options = {}) {
    const {mongos = 1, shards = 3, rsNodes = 1} = options;
    const configShard = TestData.configShard || false;
    const fsmSt = createShardingTest(mongos, shards, rsNodes, configShard);
    const fsmShards = assert.commandWorked(fsmSt.s.adminCommand({listShards: 1})).shards;
    const instancesToCleanup = [];
    const ctx = {fsmSt, fsmShards, fsmInstancesToCleanup: instancesToCleanup};
    const {commandsByWriter, startTime, createInstanceName} = setupFsmTest(ctx, testName, options);

    const writerInstanceName = instancesToCleanup.find((n) => n.startsWith("writer_"));

    return {
        fsmSt,
        fsmShards,
        commandsByWriter,
        startTime,
        createInstanceName,
        writerInstanceName,
        teardown() {
            runTeardownSteps(
                () => Writer.joinAll(),
                () => BackgroundMutator.join(),
                () => ChangeStreamReader.joinAll(),
                () => Connector.cleanupAll(fsmSt.s),
                () => fsmSt.stop(),
            );
        },
    };
}

function getShardConnections(fsmSt) {
    const conns = [];
    for (let i = 0; fsmSt[`rs${i}`]; i++) {
        conns.push(fsmSt[`rs${i}`].getPrimary());
    }
    return conns;
}

/**
 * Build reader specs from the env for the given watchMode, then run
 * verification. Readers for all specs are launched in parallel before
 * any verification begins, so multiple collections/databases read
 * concurrently.
 */
function verifyForMode(env, watchMode, verifyOpts) {
    const batchSize = TestData.batchSize || undefined;
    let writers = env.commandsByWriter;
    if (verifyOpts.primaryWriterOnly) {
        // Set by verifyResume for db_absent in collection/database mode.
        // See verifyResume for rationale.
        writers = [writers[0]];
    }
    const specs = buildReaderSpecs(writers, env.startTime, batchSize, watchMode);

    // Phase 1: launch all readers across all specs in parallel.
    const scopeContexts = specs.map((spec) => {
        const {readers, extraVerifierConfig = {}} = verifyOpts;
        const readerNamesBySuffix = {};
        const readerConfigs = {};
        const matcherSpecsByInstance = {};

        for (const {suffix, configOverrides} of readers) {
            const name = env.createInstanceName(`reader_${suffix}_${spec.label}`);
            const config = {...spec.config, ...configOverrides, instanceName: name};
            readerNamesBySuffix[suffix] = name;
            readerConfigs[name] = config;
            ChangeStreamReader.run(env.fsmSt.s, config);
            matcherSpecsByInstance[name] = spec.createMatcher();
        }

        return {spec, readerNamesBySuffix, readerConfigs, matcherSpecsByInstance, extraVerifierConfig};
    });

    // Phase 2: verify each spec (readers are already running / finished).
    for (const {
        spec,
        readerNamesBySuffix,
        readerConfigs,
        matcherSpecsByInstance,
        extraVerifierConfig,
    } of scopeContexts) {
        new Verifier().run(
            env.fsmSt.s,
            {
                changeStreamReaderConfigs: readerConfigs,
                matcherSpecsByInstance,
                instanceName: env.createInstanceName(`verifier_${spec.label}`),
                ...extraVerifierConfig,
            },
            verifyOpts.createTestCases(readerNamesBySuffix),
        );
    }
}

function verifyContinuous(env, watchMode) {
    if (TestData.ignoreRemovedShards) {
        return verifyIgnoreRemovedShards(env, watchMode, "continuous");
    }
    verifyForMode(env, watchMode, {
        readers: [{suffix: "cont", configOverrides: {}}],
        createTestCases: (m) => [new SingleReaderVerificationTestCase(m.cont)],
    });
}

function verifyResume(env, watchMode, startState) {
    if (TestData.ignoreRemovedShards) {
        return verifyIgnoreRemovedShards(env, watchMode, "resume");
    }
    const shardConnections = getShardConnections(env.fsmSt);

    // For db_absent the noise writer targets a separate database (TEST_DB_2).
    // In collection/database mode those events are invisible to the scoped
    // stream, so we skip that writer to avoid doubling shard-level drains
    // (3 resume points × N shards × 2 writers) and inflating the oplog
    // cluster time pool — risking timeouts in long suites like bg_mutator.
    // In cluster mode the stream sees all databases, so we must include both
    // writers in the matcher.
    const skipNoiseWriter = startState === State.DATABASE_ABSENT && watchMode !== ChangeStreamWatchMode.kCluster;

    verifyForMode(env, watchMode, {
        readers: [{suffix: "resume", configOverrides: {}}],
        createTestCases: (m) => [new PrefixReadTestCase(m.resume, 3)],
        extraVerifierConfig: {shardConnections},
        primaryWriterOnly: skipNoiseWriter,
    });
}

function verifyV1V2(env, watchMode) {
    verifyForMode(env, watchMode, {
        readers: [
            {suffix: "v1", configOverrides: {version: "v1"}},
            {suffix: "v2", configOverrides: {version: "v2"}},
        ],
        createTestCases: (m) => [new SequentialPairwiseFetchingTestCase(m.v1, m.v2)],
    });
}

function verifyFetchAndResume(env, watchMode) {
    if (TestData.ignoreRemovedShards) {
        return verifyIgnoreRemovedShards(env, watchMode, "foar");
    }
    verifyForMode(env, watchMode, {
        readers: [
            {suffix: "cont", configOverrides: {readingMode: ChangeStreamReadingMode.kContinuous}},
            {suffix: "foar", configOverrides: {readingMode: ChangeStreamReadingMode.kFetchOneAndResume}},
        ],
        createTestCases: (m) => [new SequentialPairwiseFetchingTestCase(m.cont, m.foar)],
    });
}

function verifyComparison(env, watchMode) {
    if (TestData.pairwiseIrs) {
        return verifyStrictVsIgnoreRemovedShards(env, watchMode);
    }
    return verifyV1V2(env, watchMode);
}

function verifyStrictVsIgnoreRemovedShards(env, watchMode) {
    verifyForMode(env, watchMode, {
        readers: [
            {suffix: "strict", configOverrides: {version: "v2"}},
            {suffix: "irs", configOverrides: {version: "v2", ignoreRemovedShards: true}},
        ],
        createTestCases: (m) => [new SequentialPairwiseFetchingTestCase(m.strict, m.irs)],
    });
}

function removeRandomShardFromSet(st, shardSet) {
    assert.gte(shardSet.length, 2, "Need at least 2 shards to remove one");
    const idx = Random.randInt(shardSet.length);
    const shardToRemove = shardSet[idx];

    jsTest.log.debug("removeRandomShardFromSet", {
        shardId: shardToRemove._id,
        totalShards: shardSet.length,
    });

    // Move ALL databases whose primary is on the shard being removed, not just
    // the test DB — the Connector control database may also live there.
    const otherShards = shardSet.filter((s) => s._id !== shardToRemove._id);
    const dbsOnShard = st.s.getDB("config").databases.find({primary: shardToRemove._id}).toArray();
    for (const dbDoc of dbsOnShard) {
        const newPrimary = otherShards[Random.randInt(otherShards.length)];
        assert.commandWorked(st.s.adminCommand({movePrimary: dbDoc._id, to: newPrimary._id}));
    }

    // Move all sharded chunks off the shard. The balancer is off
    // (assumes_balancer_off) so removeShard's automatic draining won't work.
    const configDb = st.s.getDB("config");
    const shardedColls = configDb.collections.find({}).toArray();
    for (const coll of shardedColls) {
        const chunksToMove = configDb.chunks.find({uuid: coll.uuid, shard: shardToRemove._id}).toArray();
        for (const chunk of chunksToMove) {
            const dest = otherShards[Random.randInt(otherShards.length)];
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: coll._id,
                    find: chunk.min,
                    to: dest._id,
                }),
            );
        }
    }

    moveOutSessionChunks(st, shardToRemove._id, otherShards[0]._id);
    removeShard(st, shardToRemove._id);
    return shardToRemove;
}

function verifyIgnoreRemovedShards(env, watchMode, readingMode = "continuous") {
    // Writers must finish before removing a shard so all expected events are generated.
    Connector.waitForDone(env.fsmSt.s, env.writerInstanceName);
    Writer.joinAll();

    const removedShard = removeRandomShardFromSet(env.fsmSt, env.fsmShards);

    const irsBase = {
        version: "v2",
        ignoreRemovedShards: true,
    };

    const modeMap = {
        "continuous": {
            suffix: "irs_cont",
            readingMode: ChangeStreamReadingMode.kReadUntilDone,
        },
        "foar": {
            suffix: "irs_foar",
            readingMode: ChangeStreamReadingMode.kFetchOneAndResumeUntilDone,
        },
        "resume": {
            suffix: "irs_resume",
            readingMode: ChangeStreamReadingMode.kReadUntilDone,
        },
    };
    const selected = modeMap[readingMode];
    assert(selected, `Unknown readingMode: ${readingMode}`);

    const isResume = readingMode === "resume";
    const shardConnections = isResume ? getShardConnections(env.fsmSt) : [];

    verifyForMode(env, watchMode, {
        readers: [{suffix: selected.suffix, configOverrides: {...irsBase, readingMode: selected.readingMode}}],
        createTestCases: isResume
            ? (m) => [new PrefixReadTestCase(m[selected.suffix], 3, {allowSkips: true})]
            : (m) => [new SingleReaderVerificationTestCase(m[selected.suffix], {allowSkips: true})],
        extraVerifierConfig: isResume ? {shardConnections} : {},
    });

    jsTest.log.info("FSM ignoreRemovedShards: PASSED", {
        watchMode,
        readingMode,
        removedShardId: removedShard._id,
    });
}

/**
 * Resolve the watch mode and writer configuration from TestData.watchMode.
 * @param {string} startState - Initial FSM state (e.g. State.DATABASE_ABSENT).
 * @returns {Object} { watchMode, writers } ready to pass to setupFsmCluster / verify*.
 */
function resolveWatchConfig(startState) {
    const mode = TestData.watchMode || "collection";
    const watchModeMap = {
        "collection": ChangeStreamWatchMode.kCollection,
        "database": ChangeStreamWatchMode.kDb,
        "cluster": ChangeStreamWatchMode.kCluster,
    };
    const watchMode = watchModeMap[mode];
    assert(watchMode !== undefined, `Unknown TestData.watchMode: ${mode}`);

    const writers = [{dbName: TEST_DB, collName: TEST_COLL, startState}];

    if (startState === State.DATABASE_ABSENT || mode === "cluster") {
        // Cross-db writer: tests isolation for collection/database readers,
        // and interleaving for cluster readers.
        writers.push({dbName: TEST_DB_2, collName: TEST_COLL, startState});
    } else {
        // Same-db, different collection: tests interleaving within a database.
        writers.push({dbName: TEST_DB, collName: TEST_COLL_2, startState});
    }

    return {watchMode, writers};
}

export {
    TEST_DB,
    TEST_DB_2,
    TEST_COLL,
    TEST_COLL_2,
    TEST_SEED,
    kExcludedOperationTypes,
    createShardingTest,
    setupFsmCluster,
    resolveWatchConfig,
    BackgroundMutatorOpType,
    verifyContinuous,
    verifyResume,
    verifyFetchAndResume,
    verifyComparison,
};
