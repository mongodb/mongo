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
    DropIndexCommand,
    CreateIndexCommand,
    ShardCollectionCommand,
    ReshardCollectionCommand,
    RenameToNonExistentSameDbCommand,
    UnshardCollectionCommand,
    CreateUntrackedCollectionCommand,
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
import {ChangeStreamReader, ChangeStreamReadingMode} from "jstests/libs/util/change_stream/change_stream_reader.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {Verifier, SingleReaderVerificationTestCase} from "jstests/libs/util/change_stream/change_stream_verifier.js";
import {
    TEST_DB,
    TEST_SEED,
    getCurrentClusterTime,
    createShardingTest,
    createMatcher,
    cleanupTestDatabase,
} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

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

describe("ShardingCommandGenerator", function () {
    before(() => {
        this.st = createShardingTest();
        this.shards = assert.commandWorked(this.st.s.adminCommand({listShards: 1})).shards;
    });

    after(() => {
        this.st.stop();
    });

    it("should generate identical command sequences for the same seed", () => {
        const gen1 = new ShardingCommandGenerator(TEST_SEED);
        const gen2 = new ShardingCommandGenerator(TEST_SEED);

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
        const generator = new ShardingCommandGenerator(TEST_SEED);
        const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const params = new ShardingCommandGeneratorParams("test_db_gen", "test_coll", this.shards);

        const commands = generator.generateCommands(testModel, params);

        jsTest.log.info(`Generated ${commands.length} commands (seed: ${TEST_SEED})`);

        // Verify commands were generated.
        assert.gt(commands.length, 0, "Should generate at least one command");
        for (let i = 0; i < commands.length; i++) {
            assert(commands[i].execute, `Command ${i} should have execute method`);
            assert(commands[i].toString, `Command ${i} should have toString method`);
        }
        jsTest.log.info("✓ All commands are valid");
    });

    it("should execute commands successfully using Writer", () => {
        const dbName = TEST_DB;
        const collName = "test_coll_exec";
        const instanceName = "test_instance_1";

        jsTest.log.info(`Testing command execution with Writer (seed: ${TEST_SEED})`);

        // Clean database.
        assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());

        // Set up writer config.
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const config = setupWriterConfig(TEST_SEED, params, instanceName);

        // Execute commands using Writer.
        jsTest.log.info(`Executing ${config.commands.length} commands using Writer...`);
        Writer.run(this.st.s, config);
        jsTest.log.info(`✓ Writer completed successfully`);

        // Verify completion was signaled.
        assert(Connector.isDone(this.st.s, instanceName), "Writer should have signaled completion");
        jsTest.log.info(`✓ Completion was properly signaled`);
    });

    it("should execute two Writers sequentially on different collections", () => {
        const dbName = TEST_DB;
        const collName1 = "test_coll_multi_writer1";
        const collName2 = "test_coll_multi_writer2";
        const writerA = "writer_instance_A";
        const writerB = "writer_instance_B";

        jsTest.log.info(`Testing two Writers running sequentially (seed: ${TEST_SEED})`);

        // Clean database.
        assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());

        // Set up writer configs with same seed but different collections.
        const writerAParams = new ShardingCommandGeneratorParams(dbName, collName1, this.shards);
        const writerBParams = new ShardingCommandGeneratorParams(dbName, collName2, this.shards);

        const writerAConfig = setupWriterConfig(TEST_SEED, writerAParams, writerA);
        const writerBConfig = setupWriterConfig(TEST_SEED, writerBParams, writerB);

        // Execute writers sequentially.
        Writer.run(this.st.s, writerAConfig);
        Writer.run(this.st.s, writerBConfig);

        // Verify both completed.
        assert(Connector.isDone(this.st.s, writerA), "Writer A should be done");
        assert(Connector.isDone(this.st.s, writerB), "Writer B should be done");

        // Verify both collections have same count (same command sequence).
        const testDb = this.st.s.getDB(dbName);
        const coll1 = testDb.getCollection(collName1);
        const coll2 = testDb.getCollection(collName2);
        assert.eq(
            coll1.countDocuments({}),
            coll2.countDocuments({}),
            "Both collections should have same document count",
        );

        jsTest.log.info("✓ Sequential multi-Writer test passed");
    });

    it("runs the graph mutator and exercises all FSM transitions", () => {
        const dbName = TEST_DB;
        const collName = "test_coll_fsm_transitions";

        // Clean database.
        assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());

        const model = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const generator = new ShardingCommandGenerator(TEST_SEED);
        const commands = generator.generateCommands(model, params);

        jsTest.log.info(`\n========== Graph mutator - Full FSM traversal ==========`);
        jsTest.log.info(`Seed: ${TEST_SEED}, DB: ${dbName}, Coll: ${collName}`);
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
        this.st = createShardingTest();
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
            assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());
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
        const dbName = TEST_DB;
        const collName = `test_coll_${modeName.toLowerCase()}`;
        const writerInstanceName = "writer_test";
        const readerInstanceName = "reader_test";

        ctx.instanceNamesToCleanup.push(readerInstanceName);
        ctx.databasesToCleanup.add(dbName);

        // Create collection (drop first to ensure clean state).
        const db = ctx.st.s.getDB(dbName);
        assert.commandWorked(db.dropDatabase());
        assert.commandWorked(db.createCollection(collName));

        // Get cluster time strictly AFTER the create event.
        const clusterTime = getCurrentClusterTime(ctx.st.s, dbName);

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
        // showExpandedEvents: false because this test verifies DML (insert) behavior only.
        // DDL events like 'create' from collection setup are not relevant here.
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
        const dbName = TEST_DB;
        const collName = "test_coll_invalidate";
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
            assert.commandWorked(db.dropDatabase());
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

        // Get cluster time strictly AFTER setup.
        const startTime = getCurrentClusterTime(this.st.s, dbName);
        jsTest.log.info(`Start time: ${tojson(startTime)}`);

        executeCommands();

        // Configure ChangeStreamReader.
        // showExpandedEvents: false because this test verifies DML (insert/drop/invalidate) behavior.
        // DDL events like 'create' from collection setup are not relevant here.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: expectedEventTypes.length,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
            showExpandedEvents: false,
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
        const dbName = TEST_DB;
        const collName = "test_coll_resume_invalidate";
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
            assert.commandWorked(db.dropDatabase());
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

        // Get cluster time strictly AFTER setup.
        const startTime = getCurrentClusterTime(this.st.s, dbName);

        executeCommands();

        // showExpandedEvents: false because this test verifies DML (insert/drop/invalidate) behavior.
        // DDL events like 'create' from collection setup are not relevant here.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: expectedEventTypes.length,
            readingMode: ChangeStreamReadingMode.kFetchOneAndResume,
            startAtClusterTime: startTime,
            showExpandedEvents: false,
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
        const dbName = TEST_DB;
        const collName1 = "test_coll_db_watch_a";
        const collName2 = "test_coll_db_watch_b";
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
        assert.commandWorked(db.dropDatabase());
        assert.commandWorked(db.createCollection(collName1));
        assert.commandWorked(db.createCollection(collName2));

        // Get cluster time strictly AFTER setup.
        const startTime = getCurrentClusterTime(this.st.s, dbName);

        // Execute commands using Writer.
        const writerConfig = {
            commands: commands,
            instanceName: writerInstanceName,
        };
        Writer.run(this.st.s, writerConfig);

        // showExpandedEvents: false because this test verifies DML (insert) behavior only.
        // DDL events like 'create' from collection setup are not relevant here.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kDb,
            dbName: dbName,
            collName: null, // Not needed for db-level watch.
            numberOfEventsToRead: expectedEventTypes.length,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
            showExpandedEvents: false,
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
        Random.setRandomSeed(TEST_SEED);

        const dbName = TEST_DB;
        const collName = `test_coll_ddl_${testName}`;
        const readerInstanceName = `reader_ddl_${testName}`;

        ctx.databasesToCleanup.add(dbName);
        ctx.instanceNamesToCleanup.push(readerInstanceName);

        // Clean database and flush router config.
        cleanupTestDatabase(ctx.st.s, dbName);

        // Get cluster time BEFORE running any commands so we capture all events.
        const startTime = getCurrentClusterTime(ctx.st.s, dbName);

        // Run setup commands.
        for (const cmd of setupCommands) {
            cmd.execute(ctx.st.s);
        }

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
                new CreateUntrackedCollectionCommand(TEST_DB, "test_coll_ddl_insert", this.shards, {}),
            ];
            const testCommand = new InsertDocCommand(TEST_DB, "test_coll_ddl_insert", this.shards, {
                exists: true,
                nonEmpty: false,
            });
            // Setup: create → Test: insert
            runDDLTest(this, "insert", setupCommands, testCommand, ["create", "insert"]);
        });
    });

    describe("Index", function () {
        it("createIndex emits createIndexes event", function () {
            const setupCommands = [
                new CreateUntrackedCollectionCommand(TEST_DB, "test_coll_ddl_create_index", this.shards, {}),
            ];
            const testCommand = new CreateIndexCommand(
                TEST_DB,
                "test_coll_ddl_create_index",
                this.shards,
                {exists: true},
                {data: 1}, // indexSpec
            );
            // Setup: create → Test: createIndexes
            runDDLTest(this, "create_index", setupCommands, testCommand, ["create", "createIndexes"]);
        });
    });

    describe("Sharding", function () {
        it("shardCollection (range) emits shardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_shard_coll_range",
                    this.shards,
                    {exists: false},
                    {data: 1}, // indexSpec
                ),
            ];
            const testCommand = new ShardCollectionCommand(
                TEST_DB,
                "test_coll_ddl_shard_coll_range",
                this.shards,
                {exists: true},
                {data: 1}, // shardKey
            );
            // Setup: create + createIndexes → Test: shardCollection
            runDDLTest(this, "shard_coll_range", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
            ]);
        });

        it("shardCollection (hashed) emits shardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_shard_coll_hashed",
                    this.shards,
                    {exists: false},
                    {data: "hashed"}, // indexSpec
                ),
            ];
            const testCommand = new ShardCollectionCommand(
                TEST_DB,
                "test_coll_ddl_shard_coll_hashed",
                this.shards,
                {exists: true},
                {data: "hashed"}, // shardKey
            );
            // Setup: create + createIndexes → Test: shardCollection
            runDDLTest(this, "shard_coll_hashed", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
            ]);
        });

        // TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
        // distribution, causing unpredictable event counts in multi-shard clusters.
        it.skip("reshardCollection emits reshardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_reshard",
                    this.shards,
                    {exists: false},
                    {data: 1}, // indexSpec
                ),
                new ShardCollectionCommand(
                    TEST_DB,
                    "test_coll_ddl_reshard",
                    this.shards,
                    {exists: true},
                    {data: 1}, // shardKey
                ),
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_reshard",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"}, // indexSpec for new shard key
                ),
            ];
            const testCommand = new ReshardCollectionCommand(
                TEST_DB,
                "test_coll_ddl_reshard",
                this.shards,
                {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                {data: "hashed"}, // newShardKey
            );
            // Setup: create + createIndexes + shardCollection + createIndexes → Test: reshardCollection
            runDDLTest(this, "reshard", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
                "createIndexes",
                "reshardCollection",
            ]);
        });

        it("unshardCollection emits reshardCollection event", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_unshard",
                    this.shards,
                    {exists: false},
                    {data: 1}, // indexSpec
                ),
                new ShardCollectionCommand(
                    TEST_DB,
                    "test_coll_ddl_unshard",
                    this.shards,
                    {exists: true},
                    {data: 1}, // shardKey
                ),
            ];
            const testCommand = new UnshardCollectionCommand(TEST_DB, "test_coll_ddl_unshard", this.shards, {
                exists: true,
                isSharded: true,
                shardKeySpec: {data: 1},
            });
            // Setup: create + createIndexes + shardCollection → Test: reshardCollection
            runDDLTest(this, "unshard", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
                "reshardCollection",
            ]);
        });
    });

    describe("Rename", function () {
        it("rename emits rename + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand(TEST_DB, "test_coll_ddl_rename", this.shards, {exists: false}, {data: 1}),
                new ShardCollectionCommand(TEST_DB, "test_coll_ddl_rename", this.shards, {exists: true}, {data: 1}),
            ];
            const testCommand = new RenameToNonExistentSameDbCommand(TEST_DB, "test_coll_ddl_rename", this.shards, {
                exists: true,
            });
            // Setup: create + createIndexes + shardCollection → Test: rename + invalidate
            runDDLTest(this, "rename", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
                "rename",
                "invalidate",
            ]);
        });

        // TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
        // distribution, causing unpredictable event counts in multi-shard clusters.
        it.skip("rename (resharded collection) emits rename + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_rename",
                    this.shards,
                    {exists: false},
                    {data: 1},
                ),
                new ShardCollectionCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_rename",
                    this.shards,
                    {exists: true},
                    {data: 1},
                ),
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_rename",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
                new ReshardCollectionCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_rename",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
            ];
            const testCommand = new RenameToNonExistentSameDbCommand(
                TEST_DB,
                "test_coll_ddl_resharded_rename",
                this.shards,
                {exists: true},
            );
            // Setup: create + createIndexes + shardCollection + createIndexes + reshardCollection → Test: rename + invalidate
            runDDLTest(this, "resharded_rename", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
                "createIndexes",
                "reshardCollection",
                "rename",
                "invalidate",
            ]);
        });
    });

    describe("Drop", function () {
        it("drop emits drop + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand(TEST_DB, "test_coll_ddl_drop", this.shards, {exists: false}, {data: 1}),
                new ShardCollectionCommand(TEST_DB, "test_coll_ddl_drop", this.shards, {exists: true}, {data: 1}),
            ];
            const testCommand = new DropCollectionCommand(TEST_DB, "test_coll_ddl_drop", this.shards, {
                exists: true,
            });
            // Setup: create + createIndexes + shardCollection → Test: drop + invalidate
            runDDLTest(this, "drop", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
                "drop",
                "invalidate",
            ]);
        });

        // TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
        // distribution, causing unpredictable event counts in multi-shard clusters.
        it.skip("drop (resharded collection) emits drop + invalidate", function () {
            const setupCommands = [
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_drop",
                    this.shards,
                    {exists: false},
                    {data: 1},
                ),
                new ShardCollectionCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_drop",
                    this.shards,
                    {exists: true},
                    {data: 1},
                ),
                new CreateIndexCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_drop",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
                new ReshardCollectionCommand(
                    TEST_DB,
                    "test_coll_ddl_resharded_drop",
                    this.shards,
                    {exists: true, shardKeySpec: {data: 1}, isSharded: true},
                    {data: "hashed"},
                ),
            ];
            const testCommand = new DropCollectionCommand(TEST_DB, "test_coll_ddl_resharded_drop", this.shards, {
                exists: true,
            });
            // Setup: create + createIndexes + shardCollection + createIndexes + reshardCollection → Test: drop + invalidate
            runDDLTest(this, "resharded_drop", setupCommands, testCommand, [
                "create",
                "createIndexes",
                "shardCollection",
                "createIndexes",
                "reshardCollection",
                "drop",
                "invalidate",
            ]);
        });
    });

    /*
     * ================================================================================
     * RESHARD LIFECYCLE TESTS
     * ================================================================================
     */

    /**
     * Helper: Execute commands and verify change stream events using Verifier.
     */
    function executeAndVerifyEvents(testContext, dbName, collName, commands) {
        Random.setRandomSeed(TEST_SEED);

        cleanupTestDatabase(testContext.st.s, dbName);
        testContext.databasesToCleanup.add(dbName);

        const ts = Date.now();
        const writerInstanceName = `writer_${dbName}_${ts}`;
        const readerInstanceName = `reader_${dbName}_${ts}`;
        const verifierInstanceName = `verifier_${dbName}_${ts}`;
        testContext.instanceNamesToCleanup.push(writerInstanceName, readerInstanceName, verifierInstanceName);

        // Collect expected events.
        const expectedEvents = commands
            .flatMap((cmd) => cmd.getChangeEvents(ChangeStreamWatchMode.kCollection))
            .map((e) => ({event: e, cursorClosed: e.operationType === "invalidate"}));

        // Get cluster time BEFORE executing commands.
        const startTime = getCurrentClusterTime(testContext.st.s, dbName);

        Writer.run(testContext.st.s, {commands, instanceName: writerInstanceName});

        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName,
            collName,
            readingMode: ChangeStreamReadingMode.kContinuous,
            startAtClusterTime: startTime,
            numberOfEventsToRead: expectedEvents.length,
        };

        ChangeStreamReader.run(testContext.st.s, readerConfig);

        new Verifier().run(
            testContext.st.s,
            {
                changeStreamReaderConfigs: {[readerInstanceName]: readerConfig},
                matcherSpecsByInstance: {[readerInstanceName]: createMatcher(expectedEvents)},
                instanceName: verifierInstanceName,
            },
            [new SingleReaderVerificationTestCase(readerInstanceName)],
        );
    }

    /**
     * Helper: Create initial empty collection context.
     */
    function emptyCtx() {
        return {
            exists: false,
            nonEmpty: false,
            shardKeySpec: null,
            currentShardKeySpec: null,
            isSharded: false,
            wasResharded: false,
        };
    }

    describe("Reshard Lifecycle", function () {
        // TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
        // distribution, causing unpredictable event counts in multi-shard clusters.
        it.skip("double reshard cycle", function () {
            const dbName = TEST_DB;
            const collName = "test_coll_double_reshard";
            const commands = [];
            let ctx = emptyCtx();

            // Create → Shard → Reshard → Reshard again.
            // IMPORTANT: Each command must receive a COPY of ctx ({...ctx}).
            commands.push(new CreateUntrackedCollectionCommand(dbName, collName, this.shards, {...ctx}));
            ctx.exists = true;

            // Shard with range key.
            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            commands.push(new ShardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            ctx.isSharded = true;
            ctx.shardKeySpec = {data: 1};

            // Reshard to hashed.
            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            ctx.shardKeySpec = {data: "hashed"};

            // Reshard back to range.
            // Note: {data: 1} index already exists from initial sharding, no need to create again.
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            ctx.shardKeySpec = {data: 1};

            executeAndVerifyEvents(this, dbName, collName, commands);
        });

        // TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
        // distribution, causing unpredictable event counts in multi-shard clusters.
        it.skip("first reshard", function () {
            const dbName = TEST_DB;
            const collName = "test_coll_first_reshard";
            const commands = [];
            let ctx = emptyCtx();

            // Create → Shard → Reshard (first time).
            // IMPORTANT: Each command must receive a COPY of ctx ({...ctx}).
            commands.push(new CreateUntrackedCollectionCommand(dbName, collName, this.shards, {...ctx}));
            ctx.exists = true;

            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            commands.push(new ShardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            ctx.isSharded = true;
            ctx.shardKeySpec = {data: 1};

            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            ctx.shardKeySpec = {data: "hashed"};

            executeAndVerifyEvents(this, dbName, collName, commands);
        });

        // TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
        // distribution, causing unpredictable event counts in multi-shard clusters.
        it.skip("shard → reshard → unshard lifecycle", function () {
            const dbName = TEST_DB;
            const collName = "test_coll_reshard_unshard";
            const commands = [];
            let ctx = emptyCtx();

            // IMPORTANT: Each command must receive a COPY of ctx ({...ctx}).
            commands.push(new CreateUntrackedCollectionCommand(dbName, collName, this.shards, {...ctx}));
            ctx.exists = true;

            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            commands.push(new ShardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            ctx.isSharded = true;
            ctx.shardKeySpec = {data: 1};

            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            ctx.shardKeySpec = {data: "hashed"};

            // Unshard collection.
            commands.push(new UnshardCollectionCommand(dbName, collName, this.shards, {...ctx}));

            executeAndVerifyEvents(this, dbName, collName, commands);
        });
    }); // end describe("Reshard Lifecycle")

    /*
     * ================================================================================
     * DIRECT EXECUTION RESHARD TESTS
     * ================================================================================
     *
     * These tests use direct command execution (without Writer) to verify
     * reshard event sequences with explicit context copying. Useful for debugging
     * individual reshard scenarios.
     *
     * ================================================================================
     */

    /**
     * Helper: Execute commands and verify change stream events using ChangeStreamReader.
     * Uses SingleChangeStreamMatcher for subsequence matching to handle per-shard duplicate events.
     * @param {ShardingTest} st - The sharding test instance
     * @param {string} dbName - Database name
     * @param {string} collName - Collection name
     * @param {Array} commands - Array of command objects to execute
     * @param {Array} instanceNamesToCleanup - Array to register reader instances for cleanup
     * @param {Set} databasesToCleanup - Set to register databases for cleanup
     */
    function executeAndVerifyEventsDirect(st, dbName, collName, commands, instanceNamesToCleanup, databasesToCleanup) {
        Random.setRandomSeed(TEST_SEED);
        cleanupTestDatabase(st.s, dbName);
        databasesToCleanup.add(dbName);

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

        // Get cluster time BEFORE executing commands.
        const startTime = getCurrentClusterTime(st.s, dbName);

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

        // Use deferred matching to handle extra/out-of-order events from MongoDB.
        for (const event of actualEvents) {
            matcher.matches(event, false);
        }

        // Verify all expected events were matched (extra events are logged but allowed).
        matcher.assertDone();
    }

    describe("Direct Execution Reshard", function () {
        /**
         * Test: Reshard with double reshard cycle.
         * Sequence: Create → Shard(range) → Reshard(hashed) → DropOldIndex → Reshard(range) → DropOldIndex
         *
         * IMPORTANT: Each command must receive a COPY of ctx ({...ctx}) because ctx is mutated
         * after command creation. Commands store a reference to collectionCtx, and getChangeEvents()
         * uses that reference later during verification.
         *
         * TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
         * distribution, causing unpredictable event counts in multi-shard clusters.
         */
        it.skip("double reshard cycle with index drops", function () {
            const dbName = TEST_DB;
            const collName = "test_coll_direct_double_reshard";

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
            ctx.shardKeySpec = {data: 1};

            // Step 3: Reshard to hashed key.
            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            ctx.shardKeySpec = {data: "hashed"};
            // Drop OLD range index.
            commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));

            // Step 4: Reshard back to range.
            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));
            ctx.shardKeySpec = {data: 1};
            // Drop OLD hashed index.
            commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));

            executeAndVerifyEventsDirect(
                this.st,
                dbName,
                collName,
                commands,
                this.instanceNamesToCleanup,
                this.databasesToCleanup,
            );
        });

        /**
         * Test: First reshard (no prior reshard) produces expected events.
         * Sequence: Create → Shard(range) → Reshard(hashed) → DropOldIndex
         *
         * IMPORTANT: Each command must receive a COPY of ctx ({...ctx}) because ctx is mutated
         * after command creation.
         *
         * TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
         * distribution, causing unpredictable event counts in multi-shard clusters.
         */
        it.skip("first reshard with index drop", function () {
            const dbName = TEST_DB;
            const collName = "test_coll_direct_first_reshard";

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
            ctx.shardKeySpec = {data: 1};

            // Step 3: Reshard to hashed key.
            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            ctx.shardKeySpec = {data: "hashed"};
            // Drop OLD range index.
            commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: 1}));

            executeAndVerifyEventsDirect(
                this.st,
                dbName,
                collName,
                commands,
                this.instanceNamesToCleanup,
                this.databasesToCleanup,
            );
        });

        /**
         * Test: Full shard → reshard → unshard lifecycle.
         * Sequence: Create → Shard(range) → Reshard(hashed) → DropOldIndex → Unshard → DropOldIndex
         *
         * IMPORTANT: Each command must receive a COPY of ctx ({...ctx}) because ctx is mutated
         * after command creation.
         *
         * TODO SERVER-114858: Skipped because hashed sharding has non-deterministic chunk
         * distribution, causing unpredictable event counts in multi-shard clusters.
         */
        it.skip("shard → reshard → unshard with index drop", function () {
            const dbName = TEST_DB;
            const collName = "test_coll_direct_reshard_unshard";

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
            ctx.shardKeySpec = {data: 1};

            // Step 3: Reshard to hashed key.
            commands.push(new CreateIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            commands.push(new ReshardCollectionCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));
            ctx.shardKeySpec = {data: "hashed"};
            // Drop hashed index.
            commands.push(new DropIndexCommand(dbName, collName, this.shards, {...ctx}, {data: "hashed"}));

            // Step 4: Unshard collection.
            commands.push(new UnshardCollectionCommand(dbName, collName, this.shards, {...ctx}));
            ctx.isSharded = false;
            ctx.shardKeySpec = null;

            executeAndVerifyEventsDirect(
                this.st,
                dbName,
                collName,
                commands,
                this.instanceNamesToCleanup,
                this.databasesToCleanup,
            );
        });
    }); // end describe("Direct Execution Reshard")
});
