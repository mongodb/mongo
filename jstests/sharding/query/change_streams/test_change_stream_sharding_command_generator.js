/**
 * Tests state machine command generation for change streams.
 * Validates that the ShardingCommandGenerator produces correct command sequences
 * and that Writers can execute them both sequentially and concurrently.
 *
 * @tags: [assumes_balancer_off, uses_change_streams]
 */
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {
    InsertDocCommand,
    DropCollectionCommand,
    CreateIndexCommand,
    DropIndexCommand,
    ShardCollectionCommand,
    ReshardCollectionCommand,
    RenameToNonExistentSameDbCommand,
    UnshardCollectionCommand,
    CreateUntrackedCollectionCommand,
    CreateUnsplittableCollectionCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {ShardingCommandGenerator} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator.js";
import {ShardingCommandGeneratorParams} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator_params.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ChangeEventMatcher} from "jstests/libs/util/change_stream/change_stream_event.js";
import {
    SingleChangeStreamMatcher,
    MultipleChangeStreamMatcher,
} from "jstests/libs/util/change_stream/change_stream_matcher.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

/**
 * Pseudo-command that only predicts a dropIndexes event without executing anything.
 * Useful for testing expected events that come from external sources.
 */
class ExpectDropIndexEvent {
    constructor(dbName, collName, indexSpec) {
        this.dbName = dbName;
        this.collName = collName;
        this.indexSpec = indexSpec;
    }

    execute(connection) {
        // Do nothing - we're just predicting an event, not causing one.
    }

    toString() {
        return `ExpectDropIndexEvent(${JSON.stringify(this.indexSpec)})`;
    }

    getChangeEvents(watchMode) {
        // Unsplittable collections emit 1 event (single shard).
        return [
            {
                operationType: "dropIndexes",
                ns: {db: this.dbName, coll: this.collName},
            },
        ];
    }
}

/**
 * Helper function to set up writer configuration.
 * @param {number} seed - Random seed for command generation
 * @param {ShardingCommandGeneratorParams} params - Generator parameters
 * @param {string} instanceName - Writer instance name
 * @returns {Object} Writer configuration
 */
function setupWriterConfig(seed, params, instanceName) {
    const generator = new ShardingCommandGenerator(seed);
    const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
    const commands = generator.generateCommands(testModel, params);

    jsTest.log.info(`Generated ${commands.length} commands for ${instanceName}`);

    return {
        commands: commands,
        instanceName: instanceName,
    };
}

/**
 * Get the current cluster time from the server.
 */
function getClusterTime(conn, dbName) {
    const serverStatus = conn.getDB(dbName).adminCommand({serverStatus: 1});
    return serverStatus.operationTime;
}

/**
 * Get a cluster time that is strictly after the current server time.
 * This is useful for starting change streams that should not include any
 * events that have already occurred, since startAtClusterTime is inclusive.
 */
function getClusterTimeAfterNow(conn, dbName) {
    const currentTime = getClusterTime(conn, dbName);
    // Increment the cluster time by 1 to be strictly after the current time.
    // Cluster time is Timestamp(seconds, increment), so incrementing 'i' by 1
    // gives us a time that is guaranteed to be after all current operations.
    return Timestamp(currentTime.t, currentTime.i + 1);
}

describe("ShardingCommandGenerator", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2, mongos: 1});
        this.st.stopBalancer();
        this.shards = assert.commandWorked(this.st.s.adminCommand({listShards: 1})).shards;
    });

    after(() => {
        this.st.stop();
    });

    it("should generate identical command sequences for the same seed", () => {
        const seed = 42;
        const gen1 = new ShardingCommandGenerator(seed);
        const gen2 = new ShardingCommandGenerator(seed);

        const model1 = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const model2 = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);

        const params1 = new ShardingCommandGeneratorParams("repro_db_1", "repro_coll", this.shards);
        const params2 = new ShardingCommandGeneratorParams("repro_db_2", "repro_coll", this.shards);

        const commands1 = gen1.generateCommands(model1, params1);
        const commands2 = gen2.generateCommands(model2, params2);

        assert.eq(commands1.length, commands2.length, "Same seed should produce same number of commands");

        for (let i = 0; i < commands1.length; i++) {
            assert.eq(commands1[i].toString(), commands2[i].toString(), `Command ${i}: type mismatch`);
        }
    });

    it("should generate commands", () => {
        const seed = new Date().getTime();
        const generator = new ShardingCommandGenerator(seed);
        const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const params = new ShardingCommandGeneratorParams("test_db_gen", "test_coll", this.shards);

        const commands = generator.generateCommands(testModel, params);

        jsTest.log.info(`Generated ${commands.length} commands (seed: ${seed})`);

        // Verify commands were generated.
        assert.gt(commands.length, 0, "Should generate at least one command");
        for (let i = 0; i < commands.length; i++) {
            assert(commands[i].execute, `Command ${i} should have execute method`);
            assert(commands[i].toString, `Command ${i} should have toString method`);
        }
        jsTest.log.info("✓ All commands are valid");
    });

    it("should execute commands successfully using Writer", () => {
        const testSeed = new Date().getTime();
        const dbName = "test_db_exec";
        const collName = "test_coll_exec";
        const instanceName = "test_instance_1";

        jsTest.log.info(`Testing command execution with Writer (seed: ${testSeed})`);

        const db = this.st.s.getDB(dbName);
        db.dropDatabase();

        // Set up writer config.
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const config = setupWriterConfig(testSeed, params, instanceName);

        // Execute commands using Writer.
        jsTest.log.info(`Executing ${config.commands.length} commands using Writer...`);
        Writer.run(this.st.s, config);
        jsTest.log.info(`✓ Writer completed successfully`);

        // Verify completion was signaled.
        assert(Connector.isDone(this.st.s, instanceName), "Writer should have signaled completion");
        jsTest.log.info(`✓ Completion was properly signaled`);
    });

    it("should execute two Writers sequentially on different collections", () => {
        const testSeed = 12345;
        const dbName = "test_db_multi_writer_seq";
        const collName1 = "test_coll_writer1";
        const collName2 = "test_coll_writer2";
        const writerA = "writer_instance_A";
        const writerB = "writer_instance_B";

        jsTest.log.info(`Testing two Writers running sequentially (seed: ${testSeed})`);

        const db = this.st.s.getDB(dbName);
        db.dropDatabase();

        // Set up writer configs with same seed but different collections.
        const writerAParams = new ShardingCommandGeneratorParams(dbName, collName1, this.shards);
        const writerBParams = new ShardingCommandGeneratorParams(dbName, collName2, this.shards);

        const writerAConfig = setupWriterConfig(testSeed, writerAParams, writerA);
        const writerBConfig = setupWriterConfig(testSeed, writerBParams, writerB);

        // Execute writers sequentially.
        Writer.run(this.st.s, writerAConfig);
        Writer.run(this.st.s, writerBConfig);

        // Verify both completed.
        assert(Connector.isDone(this.st.s, writerA), "Writer A should be done");
        assert(Connector.isDone(this.st.s, writerB), "Writer B should be done");

        // Verify both collections have same count (same command sequence).
        const coll1 = db.getCollection(collName1);
        const coll2 = db.getCollection(collName2);
        assert.eq(
            coll1.countDocuments({}),
            coll2.countDocuments({}),
            "Both collections should have same document count",
        );

        jsTest.log.info("✓ Sequential multi-Writer test passed");
    });

    it("runs the graph mutator and exercises all FSM transitions", () => {
        const dbName = "test_db_sharding";
        const collName = "test_coll_sharding";
        const seed = 314159;

        const db = this.st.s.getDB(dbName);
        db.dropDatabase();

        const model = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const generator = new ShardingCommandGenerator(seed);
        const commands = generator.generateCommands(model, params);

        jsTest.log.info(`\n========== Graph mutator - Full FSM traversal ==========`);
        jsTest.log.info(`Seed: ${seed}, DB: ${dbName}, Coll: ${collName}`);
        jsTest.log.info(`Total commands: ${commands.length}`);
        jsTest.log.info(`Command list:`);
        commands.forEach((cmd, idx) => {
            jsTest.log.info(`  [${idx}] ${cmd.toString()}`);
        });

        // Build set of command strings present in the generated sequence.
        const commandStrings = commands.map((c) => c.toString());

        // Collect all unique actions from the FSM model.
        const allActionsInFsm = new Set();
        for (const state of model.states) {
            for (const action of model.collectionStateToActionsMap(state).keys()) {
                allActionsInFsm.add(action);
            }
        }

        // Derive expected command patterns from the Action enum (single source of truth).
        jsTest.log.info(`\n--- Command coverage verification ---`);
        const missingCommands = [];
        for (const actionId of allActionsInFsm) {
            const commandClass = ShardingCommandGenerator.actionToCommandClass[actionId];
            assert(commandClass, `No command class mapped for action ${actionId}`);
            const commandClassName = commandClass.name;
            const actionName = Action.getName(actionId);
            const found = commandStrings.some((s) => s.includes(commandClassName));
            jsTest.log.info(`  ${actionName}: ${found ? "✓" : "✗"}`);
            if (!found) {
                missingCommands.push(actionName);
            }
        }

        // Assert all FSM actions produced their expected commands.
        assert.eq(
            missingCommands.length,
            0,
            `Missing command types: ${missingCommands.join(", ")}. All FSM actions must be covered.`,
        );

        jsTest.log.info(`  Total FSM actions: ${allActionsInFsm.size}`);
        jsTest.log.info(`  Commands generated: ${commands.length}`);
        jsTest.log.info(`✓ All FSM actions produced expected commands`);

        jsTest.log.info(`\n========== Executing commands ==========`);

        commands.forEach((cmd, cmdIdx) => {
            jsTest.log.info(`Executing [${cmdIdx}]: ${cmd.toString()}`);
            cmd.execute(this.st.s);
        });

        jsTest.log.info(`✓ All ${commands.length} commands executed successfully`);
    });
});

describe("ChangeEventMatcher helpers", function () {
    it("matches only the fields provided in the expected event and ignores extras", () => {
        const expected = {
            operationType: "insert",
            ns: {db: "testDb", coll: "testColl"},
            fullDocument: {_id: 1},
        };

        const actual = {
            operationType: "insert",
            ns: {db: "testDb", coll: "testColl"},
            fullDocument: {_id: 1},
            clusterTime: new Timestamp(1, 1), // Ignored because not in expected.
            _id: {_data: "opaqueResumeToken"}, // Ignored because not in expected.
        };

        const matcher = new ChangeEventMatcher(expected);
        assert(matcher.matches(actual));
    });

    it("returns false when a required expected field does not match", () => {
        const expected = {
            operationType: "insert",
            fullDocument: {_id: 1},
        };
        const actual = {
            operationType: "insert",
            fullDocument: {_id: 2},
        };

        const matcher = new ChangeEventMatcher(expected);
        assert(!matcher.matches(actual));
    });

    it("advances through ordered events with SingleChangeStreamMatcher", () => {
        const m1 = new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 1}});
        const m2 = new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 2}});

        const streamMatcher = new SingleChangeStreamMatcher([m1, m2]);

        assert(streamMatcher.matches({operationType: "insert", fullDocument: {_id: 1}}));
        assert(!streamMatcher.isDone());
        assert(streamMatcher.matches({operationType: "insert", fullDocument: {_id: 2}}));
        assert(streamMatcher.isDone());
    });

    it("matches interleaved events across multiple streams with MultipleChangeStreamMatcher", () => {
        // Stream 1 expects: insert _id:1, then insert _id:3.
        const stream1 = new SingleChangeStreamMatcher([
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 1}}),
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 3}}),
        ]);

        // Stream 2 expects: insert _id:2, then insert _id:4.
        const stream2 = new SingleChangeStreamMatcher([
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 2}}),
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 4}}),
        ]);

        const multiMatcher = new MultipleChangeStreamMatcher([stream1, stream2]);

        // Events arrive interleaved: 1, 2, 3, 4.
        assert(multiMatcher.matches({operationType: "insert", fullDocument: {_id: 1}}));
        assert(!multiMatcher.isDone());

        assert(multiMatcher.matches({operationType: "insert", fullDocument: {_id: 2}}));
        assert(!multiMatcher.isDone());

        assert(multiMatcher.matches({operationType: "insert", fullDocument: {_id: 3}}));
        assert(!multiMatcher.isDone());

        assert(multiMatcher.matches({operationType: "insert", fullDocument: {_id: 4}}));
        assert(multiMatcher.isDone());
    });

    it("returns false when event matches no stream in MultipleChangeStreamMatcher", () => {
        const stream1 = new SingleChangeStreamMatcher([
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 1}}),
        ]);

        const multiMatcher = new MultipleChangeStreamMatcher([stream1]);

        // Event _id:99 doesn't match any stream.
        assert(!multiMatcher.matches({operationType: "insert", fullDocument: {_id: 99}}));
    });
});

describe("ChangeStreamReader integration", function () {
    before(() => {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        this.st.stopBalancer();
        this.shards = assert.commandWorked(this.st.s.adminCommand({listShards: 1})).shards;
        // Track instance names for cleanup in afterEach.
        this.instanceNamesToCleanup = [];
        // Track databases to drop in afterEach.
        this.databasesToCleanup = new Set();
    });

    after(() => {
        this.st.stop();
    });

    afterEach(() => {
        // Clean up any change events captured during the test.
        for (const instanceName of this.instanceNamesToCleanup) {
            Connector.cleanup(this.st.s, instanceName);
        }
        this.instanceNamesToCleanup = [];

        // Drop databases used during the test.
        for (const dbName of this.databasesToCleanup) {
            this.st.s.getDB(dbName).dropDatabase();
        }
        this.databasesToCleanup.clear();
    });

    /**
     * Helper to test capturing insert events with a given reading mode.
     * @param {Object} ctx - Test context with st, shards, instanceNamesToCleanup, databasesToCleanup
     * @param {string} readingMode - Reading mode constant
     */
    function testCaptureInsertEvents(ctx, readingMode) {
        const modeName = readingMode === ChangeStreamReadingMode.kContinuous ? "Continuous" : "FetchOneAndResume";
        const dbName = `test_db_${modeName.toLowerCase()}`;
        const collName = "test_coll";
        const writerInstanceName = "writer_test";
        const readerInstanceName = "reader_test";

        ctx.instanceNamesToCleanup.push(readerInstanceName);
        ctx.databasesToCleanup.add(dbName);

        // Create collection (drop first to ensure clean state).
        const db = ctx.st.s.getDB(dbName);
        db.dropDatabase();
        assert.commandWorked(db.createCollection(collName));

        // Get cluster time strictly AFTER the create event so the change stream
        // only captures the subsequent insert events.
        const clusterTime = getClusterTimeAfterNow(ctx.st.s, dbName);

        // Execute inserts using Writer.
        const numInserts = 3;
        const insertCommands = [];
        for (let i = 0; i < numInserts; i++) {
            insertCommands.push(new InsertDocCommand(dbName, collName, ctx.shards, {exists: true, nonEmpty: i > 0}));
        }
        const writerConfig = {
            commands: insertCommands,
            instanceName: writerInstanceName,
        };
        Writer.run(ctx.st.s, writerConfig);

        // Use ChangeStreamReader with specified mode.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: numInserts,
            readingMode: readingMode,
            startAtClusterTime: clusterTime,
            showExpandedEvents: false,
        };

        ChangeStreamReader.run(ctx.st.s, readerConfig);

        // Read captured events.
        const capturedRecords = Connector.readAllChangeEvents(ctx.st.s, readerInstanceName);

        assert.eq(capturedRecords.length, numInserts, `Expected ${numInserts} events, got ${capturedRecords.length}`);
        for (let i = 0; i < numInserts; i++) {
            const event = capturedRecords[i].changeEvent;
            assert.eq(event.operationType, "insert", `Event ${i} should be insert`);
            assert(event.fullDocument._id, `Event ${i} should have _id`);
        }
    }

    it("captures insert events with Continuous mode", function () {
        testCaptureInsertEvents(this, ChangeStreamReadingMode.kContinuous);
    });

    it("captures insert events with FetchOneAndResume mode", function () {
        testCaptureInsertEvents(this, ChangeStreamReadingMode.kFetchOneAndResume);
    });

    /**
     * Test multiple inserts followed by drop (invalidate event).
     * Verifies ChangeStreamReader handles invalidate and reopens cursor correctly.
     */
    it("handles multiple inserts and invalidate event", function () {
        const dbName = "test_reader_invalidate";
        const collName = "test_coll";
        const writerInstanceName = "writer_invalidate_test";
        const readerInstanceName = "reader_invalidate_test";

        // Register for cleanup in afterEach.
        this.instanceNamesToCleanup.push(readerInstanceName);
        this.databasesToCleanup.add(dbName);

        jsTest.log.info(`\n========== ChangeStreamReader Invalidate Test ==========`);

        // Expected events: 3 inserts + drop (which triggers invalidate).
        const expectedEventTypes = ["insert", "insert", "insert", "drop", "invalidate"];

        // Build commands: 3 inserts + drop.
        const collectionCtx = {exists: true, nonEmpty: false};
        const commands = [
            new InsertDocCommand(dbName, collName, this.shards, collectionCtx),
            new InsertDocCommand(dbName, collName, this.shards, {...collectionCtx, nonEmpty: true}),
            new InsertDocCommand(dbName, collName, this.shards, {...collectionCtx, nonEmpty: true}),
            new DropCollectionCommand(dbName, collName, this.shards, collectionCtx),
        ];

        /**
         * Setup: Drop and recreate collection (drop to ensure clean state before test).
         */
        const setupCollection = () => {
            const db = this.st.s.getDB(dbName);
            db.dropDatabase();
            assert.commandWorked(db.createCollection(collName));
            return db.getCollection(collName);
        };

        /**
         * Execute commands using Writer.
         */
        const executeCommands = () => {
            const writerConfig = {
                commands: commands,
                instanceName: writerInstanceName,
            };
            Writer.run(this.st.s, writerConfig);
        };

        /**
         * Verify events match expected types.
         */
        const verifyEvents = (events) => {
            jsTest.log.info(`Verifying ${events.length} events`);
            events.forEach((event, idx) => {
                jsTest.log.info(`  [${idx}] ${event.operationType}`);
            });

            assert.eq(
                events.length,
                expectedEventTypes.length,
                `Expected ${expectedEventTypes.length} events, got ${events.length}`,
            );

            for (let i = 0; i < expectedEventTypes.length; i++) {
                assert.eq(
                    events[i].operationType,
                    expectedEventTypes[i],
                    `Event ${i} should be ${expectedEventTypes[i]}, got ${events[i].operationType}`,
                );
            }
        };

        setupCollection();
        const db = this.st.s.getDB(dbName);

        // Get cluster time strictly AFTER setup so change stream only captures test operations.
        const startTime = getClusterTimeAfterNow(this.st.s, dbName);
        jsTest.log.info(`Start time: ${tojson(startTime)}`);

        executeCommands();

        // Configure ChangeStreamReader.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: expectedEventTypes.length,
            readingMode: ChangeStreamReadingMode.kContinuous,
            showExpandedEvents: false,
            startAtClusterTime: startTime,
        };

        jsTest.log.info(`Running ChangeStreamReader.run()...`);
        ChangeStreamReader.run(this.st.s, readerConfig);

        // Read captured events from Connector.
        const capturedRecords = Connector.readAllChangeEvents(this.st.s, readerInstanceName);
        const events = capturedRecords.map((r) => r.changeEvent);

        verifyEvents(events);

        jsTest.log.info(`✓ Invalidate test passed`);
    });

    /**
     * Test FetchOneAndResume mode with invalidate.
     * This mode reopens cursor after each event, testing resume token handling.
     */
    it("handles invalidate in FetchOneAndResume mode", function () {
        const dbName = "test_reader_resume_invalidate";
        const collName = "test_coll_resume";
        const writerInstanceName = "writer_resume_inv_test";
        const readerInstanceName = "reader_resume_inv_test";

        // Register for cleanup in afterEach.
        this.instanceNamesToCleanup.push(readerInstanceName);
        this.databasesToCleanup.add(dbName);

        jsTest.log.info(`\n========== ChangeStreamReader FetchOneAndResume + Invalidate ==========`);

        // Expected events: 2 inserts + drop + invalidate.
        const expectedEventTypes = ["insert", "insert", "drop", "invalidate"];

        // Build commands: 2 inserts + drop.
        const collectionCtx = {exists: true, nonEmpty: false};
        const commands = [
            new InsertDocCommand(dbName, collName, this.shards, collectionCtx),
            new InsertDocCommand(dbName, collName, this.shards, {...collectionCtx, nonEmpty: true}),
            new DropCollectionCommand(dbName, collName, this.shards, collectionCtx),
        ];

        const setupCollection = () => {
            const db = this.st.s.getDB(dbName);
            db.dropDatabase();
            assert.commandWorked(db.createCollection(collName));
        };

        const executeCommands = () => {
            const writerConfig = {
                commands: commands,
                instanceName: writerInstanceName,
            };
            Writer.run(this.st.s, writerConfig);
        };

        const verifyEvents = (events, testName) => {
            jsTest.log.info(`${testName}: Verifying ${events.length} events`);
            events.forEach((event, idx) => {
                jsTest.log.info(`  [${idx}] ${event.operationType}`);
            });

            assert.eq(
                events.length,
                expectedEventTypes.length,
                `${testName}: Expected ${expectedEventTypes.length} events, got ${events.length}`,
            );

            for (let i = 0; i < expectedEventTypes.length; i++) {
                assert.eq(
                    events[i].operationType,
                    expectedEventTypes[i],
                    `${testName}: Event ${i} should be ${expectedEventTypes[i]}, got ${events[i].operationType}`,
                );
            }

            jsTest.log.info(`${testName}: ✓ All events verified`);
        };

        // Test with ChangeStreamReader in FetchOneAndResume mode.
        setupCollection();
        const db = this.st.s.getDB(dbName);

        // Get cluster time strictly AFTER setup so change stream only captures test operations.
        const startTime = getClusterTimeAfterNow(this.st.s, dbName);

        executeCommands();

        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: expectedEventTypes.length,
            readingMode: ChangeStreamReadingMode.kFetchOneAndResume,
            showExpandedEvents: false,
            startAtClusterTime: startTime,
        };

        jsTest.log.info(`Running ChangeStreamReader in FetchOneAndResume mode...`);
        ChangeStreamReader.run(this.st.s, readerConfig);

        const capturedRecords = Connector.readAllChangeEvents(this.st.s, readerInstanceName);
        const events = capturedRecords.map((r) => r.changeEvent);

        verifyEvents(events, "FetchOneAndResume");

        jsTest.log.info(`✓ FetchOneAndResume + Invalidate test passed`);
    });

    /**
     * Test database-level watch with multiple collections.
     */
    it("handles database-level watch with multiple collections", function () {
        const dbName = "test_reader_db_watch";
        const collName1 = "coll_a";
        const collName2 = "coll_b";
        const writerInstanceName = "writer_db_watch_test";
        const readerInstanceName = "reader_db_watch_test";

        // Register for cleanup in afterEach.
        this.instanceNamesToCleanup.push(readerInstanceName);
        this.databasesToCleanup.add(dbName);

        jsTest.log.info(`\n========== ChangeStreamReader Database Watch ==========`);

        // Expected: inserts into both collections.
        const expectedEventTypes = ["insert", "insert", "insert", "insert"];

        // Build commands: 2 inserts into each collection (interleaved).
        const collectionCtx = {exists: true, nonEmpty: false};
        const commands = [
            new InsertDocCommand(dbName, collName1, this.shards, collectionCtx),
            new InsertDocCommand(dbName, collName2, this.shards, collectionCtx),
            new InsertDocCommand(dbName, collName1, this.shards, {...collectionCtx, nonEmpty: true}),
            new InsertDocCommand(dbName, collName2, this.shards, {...collectionCtx, nonEmpty: true}),
        ];

        // Setup: drop and recreate collections (drop to ensure clean state before test).
        const db = this.st.s.getDB(dbName);
        db.dropDatabase();
        assert.commandWorked(db.createCollection(collName1));
        assert.commandWorked(db.createCollection(collName2));

        // Get cluster time strictly AFTER setup so change stream only captures test operations.
        const startTime = getClusterTimeAfterNow(this.st.s, dbName);

        // Execute commands using Writer.
        const writerConfig = {
            commands: commands,
            instanceName: writerInstanceName,
        };
        Writer.run(this.st.s, writerConfig);

        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kDb,
            dbName: dbName,
            collName: null, // Not needed for db-level watch.
            numberOfEventsToRead: expectedEventTypes.length,
            readingMode: ChangeStreamReadingMode.kContinuous,
            showExpandedEvents: false,
            startAtClusterTime: startTime,
        };

        jsTest.log.info(`Running ChangeStreamReader with database-level watch...`);
        ChangeStreamReader.run(this.st.s, readerConfig);

        const capturedRecords = Connector.readAllChangeEvents(this.st.s, readerInstanceName);

        jsTest.log.info(`Captured ${capturedRecords.length} events from database watch:`);
        capturedRecords.forEach((record, idx) => {
            const event = record.changeEvent;
            jsTest.log.info(`  [${idx}] ${event.operationType} on ${event.ns.coll}`);
        });

        assert.eq(
            capturedRecords.length,
            expectedEventTypes.length,
            `Expected ${expectedEventTypes.length} events, got ${capturedRecords.length}`,
        );

        // Verify all are inserts.
        for (const record of capturedRecords) {
            assert.eq(record.changeEvent.operationType, "insert", "All events should be inserts");
        }

        // Verify events from both collections.
        const collsWithEvents = new Set(capturedRecords.map((r) => r.changeEvent.ns.coll));
        assert(collsWithEvents.has(collName1), `Should have events from ${collName1}`);
        assert(collsWithEvents.has(collName2), `Should have events from ${collName2}`);

        jsTest.log.info(`✓ Database-level watch test passed`);
    });

    /**
     * Helper to run a DDL command and verify its change stream events.
     * @param {Object} ctx - Test context (this).
     * @param {string} testName - Unique test name (used for db/instance naming).
     * @param {Array} setupCommands - Commands to run before the test command.
     * @param {Command} testCommand - The command to test.
     * @param {Array<string>} expectedOperationTypes - Expected operationTypes for all events.
     */
    function runDDLTest(ctx, testName, setupCommands, testCommand, expectedOperationTypes) {
        const dbName = `test_ddl_${testName}`;
        const collName = "test_coll";
        const readerInstanceName = `reader_ddl_${testName}`;

        ctx.databasesToCleanup.add(dbName);
        ctx.instanceNamesToCleanup.push(readerInstanceName);

        const db = ctx.st.s.getDB(dbName);
        db.dropDatabase();

        // Run setup commands.
        for (const cmd of setupCommands) {
            cmd.execute(ctx.st.s);
        }

        // Get cluster time strictly after setup.
        const startTime = getClusterTimeAfterNow(ctx.st.s, dbName);

        // Execute test command.
        testCommand.execute(ctx.st.s);

        // Read change stream events.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: expectedOperationTypes.length,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
        };

        ChangeStreamReader.run(ctx.st.s, readerConfig);

        const capturedRecords = Connector.readAllChangeEvents(ctx.st.s, readerInstanceName);
        const capturedEvents = capturedRecords.map((r) => r.changeEvent);

        // Verify event count.
        assert.eq(
            capturedEvents.length,
            expectedOperationTypes.length,
            `Expected ${expectedOperationTypes.length} events for ${testName}, got ${capturedEvents.length}: ${tojson(capturedEvents)}`,
        );

        // Verify all event types.
        for (let i = 0; i < expectedOperationTypes.length; i++) {
            assert.eq(
                capturedEvents[i].operationType,
                expectedOperationTypes[i],
                `Event ${i} should be '${expectedOperationTypes[i]}' for ${testName}, got '${capturedEvents[i].operationType}'`,
            );
        }

        return capturedEvents;
    }

    // =========================================================================
    // Operation Tests - Verify change stream events for DML and DDL operations.
    // =========================================================================

    describe("DML", function () {
        it("insert emits insert event", function () {
            const setupCommands = [
                new CreateUntrackedCollectionCommand("test_ddl_insert", "test_coll", this.shards, {}),
            ];
            const testCommand = new InsertDocCommand("test_ddl_insert", "test_coll", this.shards, {
                exists: true,
                nonEmpty: false,
            });
            runDDLTest(this, "insert", setupCommands, testCommand, ["insert"]);
        });
    });

    describe("Index", function () {
        it("createIndex emits createIndexes event", function () {
            const setupCommands = [
                new CreateUntrackedCollectionCommand("test_ddl_create_index", "test_coll", this.shards, {}),
            ];
            const testCommand = new CreateIndexCommand(
                "test_ddl_create_index",
                "test_coll",
                this.shards,
                {exists: true},
                {data: 1}, // indexSpec
            );
            runDDLTest(this, "create_index", setupCommands, testCommand, ["createIndexes"]);
        });
    });

    describe("Sharding", function () {
        it("shardCollection (range) emits shardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    "test_ddl_shard_coll_range",
                    "test_coll",
                    this.shards,
                    {exists: false},
                    {data: 1}, // indexSpec
                ),
            ];
            const testCommand = new ShardCollectionCommand(
                "test_ddl_shard_coll_range",
                "test_coll",
                this.shards,
                {exists: true},
                {data: 1}, // shardKey
            );
            runDDLTest(this, "shard_coll_range", setupCommands, testCommand, ["shardCollection"]);
        });

        it("shardCollection (hashed) emits shardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    "test_ddl_shard_coll_hashed",
                    "test_coll",
                    this.shards,
                    {exists: false},
                    {data: "hashed"}, // indexSpec
                ),
            ];
            const testCommand = new ShardCollectionCommand(
                "test_ddl_shard_coll_hashed",
                "test_coll",
                this.shards,
                {exists: true},
                {data: "hashed"}, // shardKey
            );
            runDDLTest(this, "shard_coll_hashed", setupCommands, testCommand, ["shardCollection"]);
        });

        it("reshardCollection emits reshardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    "test_ddl_reshard",
                    "test_coll",
                    this.shards,
                    {exists: false},
                    {data: 1}, // indexSpec
                ),
                new ShardCollectionCommand(
                    "test_ddl_reshard",
                    "test_coll",
                    this.shards,
                    {exists: true},
                    {data: 1}, // shardKey
                ),
                new CreateIndexCommand(
                    "test_ddl_reshard",
                    "test_coll",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"}, // indexSpec for new shard key
                ),
            ];
            const testCommand = new ReshardCollectionCommand(
                "test_ddl_reshard",
                "test_coll",
                this.shards,
                {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                {data: "hashed"}, // newShardKey
            );
            runDDLTest(this, "reshard", setupCommands, testCommand, ["reshardCollection"]);
        });

        it("unshardCollection emits reshardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    "test_ddl_unshard",
                    "test_coll",
                    this.shards,
                    {exists: false},
                    {data: 1}, // indexSpec
                ),
                new ShardCollectionCommand(
                    "test_ddl_unshard",
                    "test_coll",
                    this.shards,
                    {exists: true},
                    {data: 1}, // shardKey
                ),
            ];
            const testCommand = new UnshardCollectionCommand("test_ddl_unshard", "test_coll", this.shards, {
                exists: true,
                isSharded: true,
                shardKeySpec: {data: 1},
            });
            runDDLTest(this, "unshard", setupCommands, testCommand, ["reshardCollection"]);
        });
    });

    describe("Rename", function () {
        it("rename emits rename + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand("test_ddl_rename", "test_coll", this.shards, {exists: false}, {data: 1}),
                new ShardCollectionCommand("test_ddl_rename", "test_coll", this.shards, {exists: true}, {data: 1}),
            ];
            const testCommand = new RenameToNonExistentSameDbCommand("test_ddl_rename", "test_coll", this.shards, {
                exists: true,
            });
            runDDLTest(this, "rename", setupCommands, testCommand, ["rename", "invalidate"]);
        });

        it("rename (resharded collection) emits rename + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    "test_ddl_resharded_rename",
                    "test_coll",
                    this.shards,
                    {exists: false},
                    {data: 1},
                ),
                new ShardCollectionCommand(
                    "test_ddl_resharded_rename",
                    "test_coll",
                    this.shards,
                    {exists: true},
                    {data: 1},
                ),
                new CreateIndexCommand(
                    "test_ddl_resharded_rename",
                    "test_coll",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
                new ReshardCollectionCommand(
                    "test_ddl_resharded_rename",
                    "test_coll",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
            ];
            const testCommand = new RenameToNonExistentSameDbCommand(
                "test_ddl_resharded_rename",
                "test_coll",
                this.shards,
                {exists: true},
            );
            runDDLTest(this, "resharded_rename", setupCommands, testCommand, ["rename", "invalidate"]);
        });
    });

    describe("Drop", function () {
        it("drop emits drop + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand("test_ddl_drop", "test_coll", this.shards, {exists: false}, {data: 1}),
                new ShardCollectionCommand("test_ddl_drop", "test_coll", this.shards, {exists: true}, {data: 1}),
            ];
            const testCommand = new DropCollectionCommand("test_ddl_drop", "test_coll", this.shards, {exists: true});
            runDDLTest(this, "drop", setupCommands, testCommand, ["drop", "invalidate"]);
        });

        it("drop (resharded collection) emits drop + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand("test_ddl_resharded_drop", "test_coll", this.shards, {exists: false}, {data: 1}),
                new ShardCollectionCommand(
                    "test_ddl_resharded_drop",
                    "test_coll",
                    this.shards,
                    {exists: true},
                    {data: 1},
                ),
                new CreateIndexCommand(
                    "test_ddl_resharded_drop",
                    "test_coll",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
                new ReshardCollectionCommand(
                    "test_ddl_resharded_drop",
                    "test_coll",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
            ];
            const testCommand = new DropCollectionCommand("test_ddl_resharded_drop", "test_coll", this.shards, {
                exists: true,
            });
            runDDLTest(this, "resharded_drop", setupCommands, testCommand, ["drop", "invalidate"]);
        });
    });

    /*
     * ================================================================================
     * FSM EVENT VERIFICATION TESTS
     * ================================================================================
     *
     * The FSM test exercises the complete state machine traversal, verifying
     * that all transitions produce expected change stream events.
     *
     * ================================================================================
     */

    describe("FSM with event matching", function () {
        it("verifies FSM-generated commands produce expected change stream events", function () {
            const dbName = "test_fsm_events";
            const collName = "test_coll";
            const seed = 42; // Fixed seed for reproducibility.
            const readerInstanceName = `reader_${dbName}_${Date.now()}`;

            this.databasesToCleanup.add(dbName);
            this.instanceNamesToCleanup.push(readerInstanceName);

            // Drop database to start clean.
            const db = this.st.s.getDB(dbName);
            db.dropDatabase();

            // Generate commands via FSM.
            const model = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
            const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
            const generator = new ShardingCommandGenerator(seed);
            const commands = generator.generateCommands(model, params);

            // Collect expected events from all commands (collection-level watch).
            const watchMode = ChangeStreamWatchMode.kCollection;
            const expectedEvents = [];
            for (const cmd of commands) {
                expectedEvents.push(...cmd.getChangeEvents(watchMode));
            }

            // Get cluster time AFTER cleanup to avoid capturing drop events.
            const startTime = getClusterTimeAfterNow(this.st.s, "admin");

            // Execute commands.
            for (const cmd of commands) {
                cmd.execute(this.st.s);
            }

            // Read events from collection-level change stream using ChangeStreamReader.
            const readerConfig = {
                instanceName: readerInstanceName,
                watchMode: watchMode,
                dbName: dbName,
                collName: collName,
                numberOfEventsToRead: expectedEvents.length,
                readingMode: ChangeStreamReadingMode.kContinuous,
                startAtClusterTime: startTime,
            };

            ChangeStreamReader.run(this.st.s, readerConfig);

            const capturedRecords = Connector.readAllChangeEvents(this.st.s, readerInstanceName);
            const actualEvents = capturedRecords.map((r) => r.changeEvent);

            // Use SingleChangeStreamMatcher for subsequence matching (handles per-shard duplicates).
            // ChangeEventMatcher uses subset matching - only compares fields present in expected.
            const eventMatchers = expectedEvents.map((e) => new ChangeEventMatcher(e));
            const matcher = new SingleChangeStreamMatcher(eventMatchers);

            for (const event of actualEvents) {
                matcher.matches(event, false);
            }

            const expectedSeq = expectedEvents.map((e) => e.operationType).join(", ");
            const actualSeq = actualEvents.map((e) => e.operationType).join(", ");
            assert(matcher.isDone(), `Expected sequence not found.\nExpected: ${expectedSeq}\nActual: ${actualSeq}`);
        });
    });

    /**
     * Helper: Execute commands and verify change stream events using ChangeStreamReader.
     * Uses SingleChangeStreamMatcher for subsequence matching to handle per-shard duplicate events.
     * @param {ShardingTest} st - The sharding test instance
     * @param {string} dbName - Database name
     * @param {string} collName - Collection name
     * @param {Array} commands - Array of command objects to execute
     * @param {Set} databasesToCleanup - Set to register databases for cleanup
     * @param {Array} instanceNamesToCleanup - Array to register reader instances for cleanup
     */
    function executeAndVerifyEvents(st, dbName, collName, commands, databasesToCleanup, instanceNamesToCleanup) {
        Random.setRandomSeed(12345);
        databasesToCleanup.add(dbName);
        const db = st.s.getDB(dbName);
        db.dropDatabase();

        const readerInstanceName = `reader_${dbName}_${Date.now()}`;
        instanceNamesToCleanup.push(readerInstanceName);

        // Collect expected events and create matchers.
        const expectedEvents = [];
        commands.forEach((cmd) => {
            expectedEvents.push(...cmd.getChangeEvents(ChangeStreamWatchMode.kCollection));
        });
        // Use subset matching via static ChangeEventMatcher.eventModifier.
        const eventMatchers = expectedEvents.map((e) => new ChangeEventMatcher(e));
        const matcher = new SingleChangeStreamMatcher(eventMatchers);

        // Get cluster time before execution.
        const startTime = getClusterTime(st.s, "admin");

        // Execute commands.
        commands.forEach((cmd) => {
            cmd.execute(st.s);
        });

        // Use ChangeStreamReader with kContinuous mode.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
            numberOfEventsToRead: expectedEvents.length,
        };

        ChangeStreamReader.run(st.s, readerConfig);

        // Read captured events from Connector.
        const capturedRecords = Connector.readAllChangeEvents(st.s, readerInstanceName);
        const actualEvents = capturedRecords.map((r) => r.changeEvent);

        // Use SingleChangeStreamMatcher for subsequence matching (handles per-shard duplicates).
        for (const event of actualEvents) {
            matcher.matches(event, false);
        }

        const expectedSeq = expectedEvents.map((e) => e.operationType).join(", ");
        const actualSeq = actualEvents.map((e) => e.operationType).join(", ");
        assert(matcher.isDone(), `Expected sequence not found.\nExpected: ${expectedSeq}\nActual: ${actualSeq}`);
    }

    /**
     * Test: Reshard with double reshard cycle.
     * Sequence: Create → Shard(range) → Reshard(hashed) → DropOldIndex → Reshard(range) → DropOldIndex
     *
     * IMPORTANT: Each command must receive a COPY of ctx ({...ctx}) because ctx is mutated
     * after command creation. Commands store a reference to collectionCtx, and getChangeEvents()
     * uses that reference later during verification.
     */
    it("verifies reshard events with double reshard cycle", function () {
        const dbName = "test_reshard_cycle";
        const collName = "test_coll";

        const commands = [];
        let ctx = {
            exists: false,
            nonEmpty: false,
            shardKeySpec: null,
            isSharded: false,
        };

        // Step 1: Create collection.
        commands.push(new CreateUntrackedCollectionCommand(dbName, collName, this.shards, {...ctx}));
        ctx.exists = true;

        // Step 2: Shard with range key {data: 1}.
        // Before sharding, collection is unsharded (1 event for createIndexes).
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        commands.push(new ShardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        ctx.isSharded = true;
        ctx.shardKeySpec = {data: 1}; // Now range-sharded (1 shard has data).

        // Step 3: Reshard to hashed key.
        // Current shard key is range (1 event for createIndexes).
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
        commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
        // After reshard, shard key is hashed (data distributed to all shards).
        ctx.shardKeySpec = {data: "hashed"};
        // Drop old range index (hashed = 2 events).
        commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));

        // Step 4: Reshard back to range.
        // Current shard key is hashed (2 events for createIndexes).
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        // After reshard, shard key is range.
        ctx.shardKeySpec = {data: 1};
        // Drop old hashed index (range = 1 event).
        commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));

        executeAndVerifyEvents(
            this.st,
            dbName,
            collName,
            commands,
            this.databasesToCleanup,
            this.instanceNamesToCleanup,
        );
    });

    /**
     * Test: First reshard (no prior reshard) produces expected events.
     * Sequence: Create → Shard(range) → Reshard(hashed) → DropOldIndex
     *
     * IMPORTANT: Each command must receive a COPY of ctx ({...ctx}) because ctx is mutated
     * after command creation.
     */
    it("verifies first reshard events", function () {
        const dbName = "test_first_reshard";
        const collName = "test_coll";

        const commands = [];
        let ctx = {
            exists: false,
            nonEmpty: false,
            shardKeySpec: null,
            isSharded: false,
        };

        // Step 1: Create collection.
        commands.push(new CreateUntrackedCollectionCommand(dbName, collName, this.shards, {...ctx}));
        ctx.exists = true;

        // Step 2: Shard with range key {data: 1}.
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        commands.push(new ShardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        ctx.isSharded = true;
        ctx.shardKeySpec = {data: 1}; // Now range-sharded.

        // Step 3: Reshard to hashed key.
        // Current shard key is range (1 event for createIndexes).
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
        commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
        // After reshard, shard key is hashed.
        ctx.shardKeySpec = {data: "hashed"};
        // Drop old range index (hashed = 2 events).
        commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));

        executeAndVerifyEvents(
            this.st,
            dbName,
            collName,
            commands,
            this.databasesToCleanup,
            this.instanceNamesToCleanup,
        );
    });

    /**
     * Test: Full shard → reshard → unshard lifecycle.
     * Sequence: Create → Shard(range) → Reshard(hashed) → DropOldIndex → Unshard → DropOldIndex
     *
     * IMPORTANT: Each command must receive a COPY of ctx ({...ctx}) because ctx is mutated
     * after command creation.
     */
    it("verifies shard → reshard → unshard events", function () {
        const dbName = "test_full_lifecycle";
        const collName = "test_coll";

        const commands = [];
        let ctx = {
            exists: false,
            nonEmpty: false,
            shardKeySpec: null,
            isSharded: false,
        };

        // Step 1: Create collection.
        commands.push(new CreateUntrackedCollectionCommand(dbName, collName, this.shards, {...ctx}));
        ctx.exists = true;

        // Step 2: Shard with range key {data: 1}.
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        commands.push(new ShardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
        ctx.isSharded = true;
        ctx.shardKeySpec = {data: 1}; // Now range-sharded.

        // Step 3: Reshard to hashed key.
        // Current shard key is range (1 event for createIndexes).
        commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
        commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
        // After reshard, shard key is hashed.
        ctx.shardKeySpec = {data: "hashed"};
        // Drop old range index (hashed = 2 events).
        commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));

        // Step 4: Unshard collection.
        // Unshard emits reshardCollection event.
        commands.push(new UnshardCollectionCommand(dbName, collName, this.shards, {...ctx}));
        // After unshard, collection is unsplittable (no longer sharded).
        ctx.isSharded = false;
        ctx.shardKeySpec = null;
        // Drop old hashed index (unsharded = 1 event).
        commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));

        executeAndVerifyEvents(
            this.st,
            dbName,
            collName,
            commands,
            this.databasesToCleanup,
            this.instanceNamesToCleanup,
        );
    });
});
