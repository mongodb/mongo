/**
 * Shared test helpers for change stream FSM tests.
 * Provides common infrastructure for setting up sharding tests, managing cluster time,
 * and running FSM-based change stream verification tests.
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
import {ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";

// Default test database name - all tests use this.
const TEST_DB = "test_cs";

// Default seed for deterministic test reproducibility.
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
 * @param {number} [shards=2] - Number of shards
 * @param {number} [rsNodes=1] - Number of replica set nodes per shard
 * @returns {ShardingTest} The configured sharding test
 */
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
 * Set up an FSM test with generated commands and reader configuration.
 * @param {Object} ctx - Test context with fsmSt, fsmShards, fsmInstancesToCleanup
 * @param {string} testName - Unique test name for logging and instance naming
 * @returns {Object} Setup result with dbName, collName, commands, expectedEvents, baseReaderConfig, createInstanceName
 */
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

    const numberOfEventsToRead = expectedEvents.length;

    jsTest.log.info(
        `FSM ${testName}: shards=${ctx.fsmShards.length}, commands=${commands.length}, ` +
            `expectedEvents=${expectedEvents.length}`,
    );

    const startTime = getCurrentClusterTime(ctx.fsmSt.s, dbName);

    const writerInstanceName = `writer_${testName}_${ts}`;
    ctx.fsmInstancesToCleanup.push(writerInstanceName);
    Writer.run(ctx.fsmSt.s, {commands, instanceName: writerInstanceName});

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
    };

    return {dbName, collName, commands, expectedEvents, baseReaderConfig, createInstanceName};
}

/**
 * Run an FSM test with automatic cluster setup and teardown.
 * Creates a ShardingTest, sets up the FSM test, runs the test function, and cleans up.
 * @param {string} testName - Unique test name
 * @param {Function} testFn - Test function receiving (fsmSt, setupResult, instancesToCleanup)
 * @param {number} [mongos=1] - Number of mongos instances
 * @param {number} [shards=1] - Number of shards
 * @param {number} [rsNodes=1] - Number of replica set nodes per shard
 */
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
