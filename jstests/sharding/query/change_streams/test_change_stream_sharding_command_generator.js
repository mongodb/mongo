/**
 * Tests state machine command generation for change streams.
 * Validates that the ShardingCommandGenerator produces correct command sequences
 * and that Writers can execute them both sequentially and concurrently.
 *
 * @tags: [uses_change_streams]
 */
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {ShardingCommandGenerator} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator.js";
import {ShardingCommandGeneratorParams} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator_params.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

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
        const controlDbName = "control_db";
        const notificationCollName = "notifications";

        jsTest.log.info(`Testing command execution with Writer (seed: ${testSeed})`);

        const db = this.st.s.getDB(dbName);
        db.dropDatabase();

        // Set up writer config
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const config = setupWriterConfig(testSeed, params, instanceName);

        // Execute commands using Writer
        jsTest.log.info(`Executing ${config.commands.length} commands using Writer...`);
        Writer.run(this.st.s, controlDbName, notificationCollName, config);
        jsTest.log.info(`✓ Writer completed successfully`);

        // Verify completion was signaled
        assert(
            Connector.isDone(this.st.s, controlDbName, notificationCollName, instanceName),
            "Writer should have signaled completion",
        );
        jsTest.log.info(`✓ Completion was properly signaled`);
    });

    it("should execute two Writers sequentially on different collections", () => {
        const testSeed = 12345;
        const dbName = "test_db_multi_writer_seq";
        const collName1 = "test_coll_writer1";
        const collName2 = "test_coll_writer2";
        const writerA = "writer_instance_A";
        const writerB = "writer_instance_B";
        const controlDbName = "control_db_seq";
        const notificationCollName = "notifications_seq";

        jsTest.log.info(`Testing two Writers running sequentially (seed: ${testSeed})`);

        const db = this.st.s.getDB(dbName);
        db.dropDatabase();

        // Set up writer configs with same seed but different collections
        const writerAParams = new ShardingCommandGeneratorParams(dbName, collName1, this.shards);
        const writerBParams = new ShardingCommandGeneratorParams(dbName, collName2, this.shards);

        const writerAConfig = setupWriterConfig(testSeed, writerAParams, writerA);
        const writerBConfig = setupWriterConfig(testSeed, writerBParams, writerB);

        // Execute writers sequentially
        Writer.run(this.st.s, controlDbName, notificationCollName, writerAConfig);
        Writer.run(this.st.s, controlDbName, notificationCollName, writerBConfig);

        // Verify both completed
        assert(Connector.isDone(this.st.s, controlDbName, notificationCollName, writerA), "Writer A should be done");
        assert(Connector.isDone(this.st.s, controlDbName, notificationCollName, writerB), "Writer B should be done");

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
});
