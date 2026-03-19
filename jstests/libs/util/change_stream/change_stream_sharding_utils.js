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
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";

// Default test database name - all tests use this.
const TEST_DB = "test_cs";

// TODO SERVER-121182: Replace with a random seed (already logged in setupFsmTest).
const TEST_SEED = 42;

/**
 * Operation types to filter out before comparison.
 * These events have unpredictable behavior in multi-shard clusters:
 * - createIndexes: Emitted per-shard, count depends on shard distribution
 * - dropIndexes: Emitted per-shard, count depends on shard distribution
 */
const kExcludedOperationTypes = ["createIndexes", "dropIndexes"];

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
 * @returns {ShardingTest} The configured sharding test
 */
function createShardingTest(mongos = 1, shards = 3, rsNodes = 1) {
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
            configOptions: {
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                },
            },
        },
    });
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
 * Drop the specified database.
 * @param {Mongo} mongos - The mongos connection
 * @param {string} [dbName=TEST_DB] - Database name to drop
 */
function cleanupTestDatabase(mongos, dbName = TEST_DB) {
    assert.commandWorked(mongos.getDB(dbName).dropDatabase());
}

/**
 * Compute expected change stream events from commands (after Writer.run()) and log them.
 * Event prediction runs after execution so move commands can use runtime state.
 * @param {string} testName - Test name for logging
 * @param {Array} commands - Executed commands with collectionCtx and getChangeEvents()
 * @returns {Array<{event: Object, cursorClosed: boolean}>} Expected events for the matcher
 */
function computeAndLogExpectedEvents(testName, commands) {
    const expectedEvents = commands
        .flatMap((cmd) => cmd.getChangeEvents(ChangeStreamWatchMode.kCollection))
        .map((e) => ({event: e, cursorClosed: e.operationType === "invalidate"}));

    jsTest.log.info("FSM expected events", {
        testName,
        count: expectedEvents.length,
        types: expectedEvents.map((e) => e.event.operationType),
    });
    for (let i = 0; i < commands.length; i++) {
        const cmd = commands[i];
        const events = cmd.getChangeEvents(ChangeStreamWatchMode.kCollection);
        jsTest.log.info("FSM cmd", {
            testName,
            cmdIndex: i,
            cmdName: cmd.constructor.name,
            events: events.map((e) => e.operationType),
            ctx: cmd.collectionCtx ?? null,
        });
    }
    return expectedEvents;
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
 * Set up an FSM test with generated commands and reader configuration.
 * @param {Object} ctx - Test context with fsmSt, fsmShards, fsmInstancesToCleanup
 * @param {string} testName - Unique test name for logging and instance naming
 * @param {Object} [options] - Optional configuration
 * @param {number} [options.startState] - FSM start state (default: DATABASE_ABSENT)
 * @returns {Object} Setup result with dbName, collName, commands, expectedEvents, baseReaderConfig, createInstanceName
 */
function setupFsmTest(ctx, testName, options = {}) {
    const startState = options.startState || State.DATABASE_ABSENT;
    jsTest.log.info(`FSM ${testName}: using seed ${TEST_SEED}`);
    Random.setRandomSeed(TEST_SEED);
    const dbName = TEST_DB;
    const collName = "test_coll_fsm";
    const ts = Date.now();

    const params = new ShardingCommandGeneratorParams(dbName, collName, ctx.fsmShards);
    const generator = new ShardingCommandGenerator(TEST_SEED);
    const testModel = new CollectionTestModel(startState);

    // Ensure database exists for non-absent start states.
    if (startState !== State.DATABASE_ABSENT) {
        ctx.fsmSt.s.getDB(dbName).dropDatabase();
        new CreateDatabaseCommand(dbName, collName, ctx.fsmShards).execute(ctx.fsmSt.s);
    }

    const commands = generator.generateCommands(testModel, params);

    jsTest.log.info("FSM setup", {
        testName,
        shards: ctx.fsmShards.length,
        commands: commands.length,
    });

    const startTime = getCurrentClusterTime(ctx.fsmSt.s, dbName);

    const writerInstanceName = `writer_${testName}_${ts}`;
    ctx.fsmInstancesToCleanup.push(writerInstanceName);
    Writer.run(ctx.fsmSt.s, writerInstanceName, commands, TEST_SEED);

    const expectedEvents = computeAndLogExpectedEvents(testName, commands);
    const commandTrace = buildCommandTrace(commands);
    const numberOfEventsToRead = expectedEvents.length;

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
        numberOfEventsToRead,
        excludeOperationTypes: kExcludedOperationTypes,
        // Used by verifier to print richer mismatch diagnostics.
        debugCommandTrace: commandTrace,
    };

    return {dbName, collName, commands, expectedEvents, commandTrace, baseReaderConfig, createInstanceName};
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
 * @param {Function} testFn - Test function receiving (fsmSt, setupResult, instancesToCleanup)
 * @param {Object} [options] - Optional configuration
 * @param {number} [options.mongos=1] - Number of mongos instances
 * @param {number} [options.shards=3] - Number of shards
 * @param {number} [options.rsNodes=1] - Number of replica set nodes per shard
 * @param {number} [options.startState] - FSM start state (passed to setupFsmTest)
 */
function runWithFsmCluster(testName, testFn, options = {}) {
    const {mongos = 1, shards = 3, rsNodes = 1} = options;
    const fsmSt = createShardingTest(mongos, shards, rsNodes);
    const fsmShards = assert.commandWorked(fsmSt.s.adminCommand({listShards: 1})).shards;
    const instancesToCleanup = [];

    try {
        const ctx = {fsmSt, fsmShards, fsmInstancesToCleanup: instancesToCleanup};
        const setupResult = setupFsmTest(ctx, testName, options);
        testFn(fsmSt, setupResult, instancesToCleanup);
    } finally {
        runTeardownSteps(
            () => Writer.joinAll(),
            () => ChangeStreamReader.joinAll(),
            ...instancesToCleanup.map((name) => () => Connector.cleanup(fsmSt.s, name)),
            () => fsmSt.s.getDB(TEST_DB).dropDatabase(),
            () => fsmSt.stop(),
        );
    }
}

export {
    TEST_DB,
    TEST_SEED,
    kExcludedOperationTypes,
    getCurrentClusterTime,
    createShardingTest,
    createMatcher,
    cleanupTestDatabase,
    setupFsmTest,
    runWithFsmCluster,
};
