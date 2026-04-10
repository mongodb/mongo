/**
 * Tests state machine command generation for change streams.
 * Validates that the ShardingCommandGenerator produces correct command sequences
 * and that Writers can execute them both sequentially and concurrently.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   # The test spins up a multi-shard cluster and runs DDL commands; too slow for
 *   # sanitizer builds that add significant overhead.
 *   incompatible_aubsan,
 *   tsan_incompatible,
 *   uses_change_streams,
 * ]
 */
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
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
import {ChangeStreamWatchMode, getClusterTime} from "jstests/libs/query/change_stream_util.js";
import {InsertDocCommand, DropCollectionCommand} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {TEST_DB, TEST_SEED, createShardingTest} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

jsTest.log.info(
    `test_change_stream_sharding_command_generator: using seed ${TEST_SEED} (to reproduce, rerun with this seed)`,
);

/**
 * Helper function to set up writer configuration.
 * @param {number} seed - Random seed for command generation
 * @param {ShardingCommandGeneratorParams} params - Generator parameters
 * @param {string} instanceName - Writer instance name
 * @returns {Object} Writer configuration
 */
function generateCommands(seed, params) {
    const generator = new ShardingCommandGenerator(seed);
    const testModel = new CollectionTestModel(State.DATABASE_ABSENT);
    return generator.generateCommands(testModel, params);
}

describe("ShardingCommandGenerator", function () {
    before(() => {
        this.st = createShardingTest();
        this.shards = assert.commandWorked(this.st.s.adminCommand({listShards: 1})).shards;
    });

    after(() => {
        this.st.stop();
    });

    afterEach(() => {
        Writer.joinAll();
    });

    it("should generate identical command sequences for the same seed", () => {
        const gen1 = new ShardingCommandGenerator(TEST_SEED);
        const gen2 = new ShardingCommandGenerator(TEST_SEED);

        const model1 = new CollectionTestModel(State.DATABASE_ABSENT);
        const model2 = new CollectionTestModel(State.DATABASE_ABSENT);

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
        const testModel = new CollectionTestModel(State.DATABASE_ABSENT);
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

        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const commands = generateCommands(TEST_SEED, params);

        jsTest.log.debug(`Executing ${commands.length} commands using Writer...`);
        Writer.run(this.st.s, instanceName, commands, TEST_SEED);
        Connector.waitForDone(this.st.s, instanceName);
        jsTest.log.info(`✓ Writer completed successfully`);
    });

    it("should execute two Writers in parallel on different databases", function () {
        const dbNameA = TEST_DB + "_writer_a";
        const dbNameB = TEST_DB + "_writer_b";
        const collName = "test_coll";
        const writerA = "writer_instance_A";
        const writerB = "writer_instance_B";

        jsTest.log.info(`Testing two Writers running in parallel (seed: ${TEST_SEED})`);

        assert.commandWorked(this.st.s.getDB(dbNameA).dropDatabase());
        assert.commandWorked(this.st.s.getDB(dbNameB).dropDatabase());

        // Separate databases so DropDatabaseCommand in one writer doesn't affect the other.
        const writerAParams = new ShardingCommandGeneratorParams(dbNameA, collName, this.shards);
        const writerBParams = new ShardingCommandGeneratorParams(dbNameB, collName, this.shards);

        const commandsA = generateCommands(TEST_SEED, writerAParams);
        const commandsB = generateCommands(TEST_SEED, writerBParams);

        Writer.run(this.st.s, writerA, commandsA, TEST_SEED);
        Writer.run(this.st.s, writerB, commandsB, TEST_SEED);

        Connector.waitForDone(this.st.s, writerA);
        const countA = this.st.s.getDB(dbNameA).getCollection(collName).countDocuments({});
        Connector.waitForDone(this.st.s, writerB);
        const countB = this.st.s.getDB(dbNameB).getCollection(collName).countDocuments({});

        assert.eq(countA, countB, "Both writers should produce same document count");

        assert.commandWorked(this.st.s.getDB(dbNameA).dropDatabase());
        assert.commandWorked(this.st.s.getDB(dbNameB).dropDatabase());

        jsTest.log.info("✓ Parallel multi-Writer test passed");
    });

    it("runs the graph mutator and exercises all FSM transitions", () => {
        const dbName = TEST_DB;
        const collName = "test_coll_fsm_transitions";

        // Clean database.
        assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());

        const model = new CollectionTestModel(State.DATABASE_ABSENT);
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const generator = new ShardingCommandGenerator(TEST_SEED);
        const commands = generator.generateCommands(model, params);

        jsTest.log.debug(`\n========== Graph mutator - Full FSM traversal ==========`);
        jsTest.log.info(`Seed: ${TEST_SEED}, DB: ${dbName}, Coll: ${collName}`);
        jsTest.log.debug(`Total commands: ${commands.length}`);
        jsTest.log.debug(`Command list:`);
        commands.forEach((cmd, idx) => {
            jsTest.log.debug(`  [${idx}] ${cmd.toString()}`);
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
        jsTest.log.debug(`\n--- Command coverage verification ---`);
        const missingCommands = [];
        for (const actionId of allActionsInFsm) {
            const commandClass = ShardingCommandGenerator.actionToCommandClass[actionId];
            assert(commandClass, `No command class mapped for action ${actionId}`);
            const commandClassName = commandClass.name;
            const actionName = Action.getName(actionId);
            const found = commandStrings.some((s) => s.includes(commandClassName));
            jsTest.log.debug(`  ${actionName}: ${found ? "✓" : "✗"}`);
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

        jsTest.log.debug(`  Total FSM actions: ${allActionsInFsm.size}`);
        jsTest.log.debug(`  Commands generated: ${commands.length}`);
        jsTest.log.info(`✓ All FSM actions produced expected commands`);

        jsTest.log.debug(`\n========== Executing commands ==========`);

        commands.forEach((cmd, cmdIdx) => {
            jsTest.log.debug(`Executing [${cmdIdx}]: ${cmd.toString()}`);
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
        Writer.joinAll();
        ChangeStreamReader.joinAll();

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
        const clusterTime = getClusterTime(ctx.st.s.getDB("admin"));

        // Execute inserts using Writer. Each InsertDocCommand inserts numDocs documents.
        const numCommands = 3;
        const numTotalInserts = numCommands * InsertDocCommand.numDocs;
        const insertCommands = [];
        for (let i = 0; i < numCommands; i++) {
            insertCommands.push(new InsertDocCommand(dbName, collName, ctx.shards, {exists: true, nonEmpty: i > 0}));
        }
        Writer.run(ctx.st.s, writerInstanceName, insertCommands, TEST_SEED);

        // showExpandedEvents: false because this test verifies DML (insert) behavior only.
        const readerConfig = {
            instanceName: readerInstanceName,
            watchMode: ChangeStreamWatchMode.kCollection,
            dbName: dbName,
            collName: collName,
            numberOfEventsToRead: numTotalInserts,
            readingMode: readingMode,
            startAtClusterTime: clusterTime,
            showExpandedEvents: false,
        };

        ChangeStreamReader.run(ctx.st.s, readerConfig);
        Connector.waitForDone(ctx.st.s, readerInstanceName);

        // Read captured events.
        const capturedRecords = Connector.readAllChangeEvents(ctx.st.s, readerInstanceName);

        assert.eq(
            capturedRecords.length,
            numTotalInserts,
            `Expected ${numTotalInserts} events, got ${capturedRecords.length}`,
        );
        for (let i = 0; i < numTotalInserts; i++) {
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

        jsTest.log.debug(`\n========== ChangeStreamReader Invalidate Test ==========`);

        // Expected events: 3 InsertDocCommands (1 insert each) + drop (which triggers invalidate).
        const expectedEventTypes = ["insert", "insert", "insert", "drop", "invalidate"];

        // Build commands: 3 InsertDocCommands + drop.
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

        const executeCommands = () => {
            Writer.run(this.st.s, writerInstanceName, commands, TEST_SEED);
        };

        const verifyEvents = (events) => {
            jsTest.log.debug(`Verifying ${events.length} events`);
            events.forEach((event, idx) => {
                jsTest.log.debug(`  [${idx}] ${event.operationType}`);
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
        const startTime = getClusterTime(this.st.s.getDB("admin"));
        jsTest.log.debug(`Start time: ${tojson(startTime)}`);

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

        jsTest.log.debug(`Running ChangeStreamReader.run()...`);
        ChangeStreamReader.run(this.st.s, readerConfig);
        Connector.waitForDone(this.st.s, readerInstanceName);

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

        jsTest.log.debug(`\n========== ChangeStreamReader FetchOneAndResume + Invalidate ==========`);

        // Expected events: 2 InsertDocCommands (1 insert each) + drop + invalidate.
        const expectedEventTypes = ["insert", "insert", "drop", "invalidate"];

        // Build commands: 2 InsertDocCommands + drop.
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
            Writer.run(this.st.s, writerInstanceName, commands, TEST_SEED);
        };

        const verifyEvents = (events, testName) => {
            jsTest.log.debug(`${testName}: Verifying ${events.length} events`);
            events.forEach((event, idx) => {
                jsTest.log.debug(`  [${idx}] ${event.operationType}`);
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

            jsTest.log.info(`✓ ${testName}: All events verified`);
        };

        // Test with ChangeStreamReader in FetchOneAndResume mode.
        setupCollection();
        const db = this.st.s.getDB(dbName);

        // Get cluster time strictly AFTER setup.
        const startTime = getClusterTime(this.st.s.getDB("admin"));

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

        jsTest.log.debug(`Running ChangeStreamReader in FetchOneAndResume mode...`);
        ChangeStreamReader.run(this.st.s, readerConfig);
        Connector.waitForDone(this.st.s, readerInstanceName);

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

        jsTest.log.debug(`\n========== ChangeStreamReader Database Watch ==========`);

        // Expected: 4 InsertDocCommands (1 insert each) into both collections.
        const expectedEventTypes = ["insert", "insert", "insert", "insert"];

        // Build commands: 2 InsertDocCommands into each collection (interleaved).
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
        const startTime = getClusterTime(this.st.s.getDB("admin"));

        Writer.run(this.st.s, writerInstanceName, commands, TEST_SEED);

        // showExpandedEvents: false because this test verifies DML (insert) behavior only.
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

        jsTest.log.debug(`Running ChangeStreamReader with database-level watch...`);
        ChangeStreamReader.run(this.st.s, readerConfig);
        Connector.waitForDone(this.st.s, readerInstanceName);

        const capturedRecords = Connector.readAllChangeEvents(this.st.s, readerInstanceName);

        jsTest.log.debug(`Captured ${capturedRecords.length} events from database watch:`);
        capturedRecords.forEach((record, idx) => {
            const event = record.changeEvent;
            jsTest.log.debug(`  [${idx}] ${event.operationType} on ${event.ns.coll}`);
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
});
