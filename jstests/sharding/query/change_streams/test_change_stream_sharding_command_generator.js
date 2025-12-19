/**
 * Tests state machine command generation for change streams.
 * Validates that the ShardingCommandGenerator produces correct command sequences
 * and that Writers can execute them both sequentially and concurrently.
 *
 * @tags: [uses_change_streams]
 */
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {InsertDocCommand, DropCollectionCommand} from "jstests/libs/util/change_stream/change_stream_commands.js";
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

describe("ShardingCommandGenerator", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2, mongos: 1});
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

        // Verify commands were generated
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

        // Set up writer config
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const config = setupWriterConfig(testSeed, params, instanceName);

        // Execute commands using Writer
        jsTest.log.info(`Executing ${config.commands.length} commands using Writer...`);
        Writer.run(this.st.s, config);
        jsTest.log.info(`✓ Writer completed successfully`);

        // Verify completion was signaled
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

        // Set up writer configs with same seed but different collections
        const writerAParams = new ShardingCommandGeneratorParams(dbName, collName1, this.shards);
        const writerBParams = new ShardingCommandGeneratorParams(dbName, collName2, this.shards);

        const writerAConfig = setupWriterConfig(testSeed, writerAParams, writerA);
        const writerBConfig = setupWriterConfig(testSeed, writerBParams, writerB);

        // Execute writers sequentially
        Writer.run(this.st.s, writerAConfig);
        Writer.run(this.st.s, writerBConfig);

        // Verify both completed
        assert(Connector.isDone(this.st.s, writerA), "Writer A should be done");
        assert(Connector.isDone(this.st.s, writerB), "Writer B should be done");

        // Verify both collections have same count (same command sequence)
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
            clusterTime: new Timestamp(1, 1), // ignored because not in expected
            _id: {_data: "opaqueResumeToken"}, // ignored because not in expected
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
        // Stream 1 expects: insert _id:1, then insert _id:3
        const stream1 = new SingleChangeStreamMatcher([
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 1}}),
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 3}}),
        ]);

        // Stream 2 expects: insert _id:2, then insert _id:4
        const stream2 = new SingleChangeStreamMatcher([
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 2}}),
            new ChangeEventMatcher({operationType: "insert", fullDocument: {_id: 4}}),
        ]);

        const multiMatcher = new MultipleChangeStreamMatcher([stream1, stream2]);

        // Events arrive interleaved: 1, 2, 3, 4
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

        // Event _id:99 doesn't match any stream
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
        this.shards = assert.commandWorked(this.st.s.adminCommand({listShards: 1})).shards;
        // Track instance names for cleanup in afterEach
        this.instanceNamesToCleanup = [];
        // Track databases to drop in afterEach
        this.databasesToCleanup = new Set();
    });

    after(() => {
        this.st.stop();
    });

    afterEach(() => {
        // Clean up any change events captured during the test
        for (const instanceName of this.instanceNamesToCleanup) {
            Connector.cleanup(this.st.s, instanceName);
        }
        this.instanceNamesToCleanup = [];

        // Drop databases used during the test
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

        // Create collection (drop first to ensure clean state)
        const db = ctx.st.s.getDB(dbName);
        db.dropDatabase();
        assert.commandWorked(db.createCollection(collName));

        // Get cluster time BEFORE Writer runs
        const clusterTime = getClusterTime(ctx.st.s, dbName);

        // Execute inserts using Writer
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

        // Use ChangeStreamReader with specified mode
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

        // Read captured events
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

        // Register for cleanup in afterEach
        this.instanceNamesToCleanup.push(readerInstanceName);
        this.databasesToCleanup.add(dbName);

        jsTest.log.info(`\n========== ChangeStreamReader Invalidate Test ==========`);

        // Expected events: 3 inserts + drop (which triggers invalidate)
        const expectedEventTypes = ["insert", "insert", "insert", "drop", "invalidate"];

        // Build commands: 3 inserts + drop
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

        // Get cluster time BEFORE operations
        const startTime = getClusterTime(this.st.s, dbName);
        jsTest.log.info(`Start time: ${tojson(startTime)}`);

        executeCommands();

        // Configure ChangeStreamReader
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

        // Read captured events from Connector
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

        // Register for cleanup in afterEach
        this.instanceNamesToCleanup.push(readerInstanceName);
        this.databasesToCleanup.add(dbName);

        jsTest.log.info(`\n========== ChangeStreamReader FetchOneAndResume + Invalidate ==========`);

        // Expected events: 2 inserts + drop + invalidate
        const expectedEventTypes = ["insert", "insert", "drop", "invalidate"];

        // Build commands: 2 inserts + drop
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

        // Test with ChangeStreamReader in FetchOneAndResume mode
        setupCollection();
        const db = this.st.s.getDB(dbName);

        const startTime = getClusterTime(this.st.s, dbName);

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

        // Register for cleanup in afterEach
        this.instanceNamesToCleanup.push(readerInstanceName);
        this.databasesToCleanup.add(dbName);

        jsTest.log.info(`\n========== ChangeStreamReader Database Watch ==========`);

        // Expected: inserts into both collections
        const expectedEventTypes = ["insert", "insert", "insert", "insert"];

        // Build commands: 2 inserts into each collection (interleaved)
        const collectionCtx = {exists: true, nonEmpty: false};
        const commands = [
            new InsertDocCommand(dbName, collName1, this.shards, collectionCtx),
            new InsertDocCommand(dbName, collName2, this.shards, collectionCtx),
            new InsertDocCommand(dbName, collName1, this.shards, {...collectionCtx, nonEmpty: true}),
            new InsertDocCommand(dbName, collName2, this.shards, {...collectionCtx, nonEmpty: true}),
        ];

        // Setup: drop and recreate collections (drop to ensure clean state before test)
        const db = this.st.s.getDB(dbName);
        db.dropDatabase();
        assert.commandWorked(db.createCollection(collName1));
        assert.commandWorked(db.createCollection(collName2));

        const startTime = getClusterTime(this.st.s, dbName);

        // Execute commands using Writer
        const writerConfig = {
            commands: commands,
            instanceName: writerInstanceName,
        };
        Writer.run(this.st.s, writerConfig);

        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kDb,
            dbName: dbName,
            collName: null, // Not needed for db-level watch
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

        // Verify all are inserts
        for (const record of capturedRecords) {
            assert.eq(record.changeEvent.operationType, "insert", "All events should be inserts");
        }

        // Verify events from both collections
        const collsWithEvents = new Set(capturedRecords.map((r) => r.changeEvent.ns.coll));
        assert(collsWithEvents.has(collName1), `Should have events from ${collName1}`);
        assert(collsWithEvents.has(collName2), `Should have events from ${collName2}`);

        jsTest.log.info(`✓ Database-level watch test passed`);
    });
});
