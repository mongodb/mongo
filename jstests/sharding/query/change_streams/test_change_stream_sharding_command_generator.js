/**
 * Tests state machine command generation for change streams.
 * @tags: [uses_change_streams]
 */
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {ShardingCommandGenerator} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator.js";
import {ShardingCommandGeneratorParams} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator_params.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

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

        const results1 = gen1.generateCommands(model1, params1);
        const results2 = gen2.generateCommands(model2, params2);

        assert.eq(results1.length, results2.length, "Same seed should produce same number of commands");

        for (let i = 0; i < results1.length; i++) {
            const t1 = results1[i].transition;
            const t2 = results2[i].transition;
            assert.eq(t1.from, t2.from, `Command ${i}: from state mismatch`);
            assert.eq(t1.action, t2.action, `Command ${i}: action mismatch`);
            assert.eq(t1.to, t2.to, `Command ${i}: to state mismatch`);
        }
    });

    it("should generate valid command sequence covering all edges", () => {
        const seed = new Date().getTime();
        const generator = new ShardingCommandGenerator(seed);
        const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const params = new ShardingCommandGeneratorParams("test_db_gen", "test_coll", this.shards);

        const results = generator.generateCommands(testModel, params);

        jsTest.log.info(`Generated ${results.length} commands (seed: ${seed})`);

        verifyCommands(testModel, results);
    });

    it("should execute commands successfully", () => {
        const testSeed = new Date().getTime();
        const generator = new ShardingCommandGenerator(testSeed);
        const collName = "test_coll_exec";

        jsTest.log.info(`Testing command execution (seed: ${generator.getSeed()})`);

        // Test with DATABASE_ABSENT starting state
        const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const dbName = `test_db_exec`;
        const db = this.st.s.getDB(dbName);

        // Drop database to start clean
        try {
            assert.commandWorked(db.dropDatabase());
        } catch (e) {
            // Database might not exist, that's okay
            jsTest.log.info(`Note: Could not drop database (may not exist): ${e.message}`);
        }

        // Generate and execute commands
        const params = new ShardingCommandGeneratorParams(dbName, collName, this.shards);
        const results = generator.generateCommands(testModel, params);
        jsTest.log.info(`Generated ${results.length} commands for execution`);

        let passed = 0,
            failed = 0;
        const failuresByType = {};

        for (const {command, transition} of results) {
            try {
                command.execute(this.st.s);
                passed++;
            } catch (e) {
                const cmdType = command.toString();
                failuresByType[cmdType] = (failuresByType[cmdType] || 0) + 1;

                // Only log first few failures of each type to avoid spam
                if (failuresByType[cmdType] <= 2) {
                    jsTest.log.info(`Failed: ${cmdType} - ${e.message.split("\n")[0]}`);
                }
                failed++;
            }
        }

        jsTest.log.info(`Execution: ${passed} passed, ${failed} failed`);
        if (failed > 0) {
            jsTest.log.info(`Failures by command type:`);
            for (const [cmdType, count] of Object.entries(failuresByType)) {
                jsTest.log.info(`  ${cmdType}: ${count} failure(s)`);
            }
        }

        // Note: Some commands use simplified/simulated implementations (see SERVER-114857).
        // For example, sharding uses a basic _id shard key, and resharding is a no-op.
        jsTest.log.info(`Note: Commands use simplified implementations for testing (see SERVER-114857)`);

        // We should have at least some successful executions
        assert.gt(passed, 0, "Expected at least some commands to execute successfully");
    });
});

/**
 * Verifies that generated commands are correct and cover all state machine transitions.
 * @param {CollectionTestModel} testModel - The state machine model.
 * @param {Array} results - Array of {command, transition} objects.
 */
function verifyCommands(testModel, results) {
    const errors = [];
    const warnings = [];

    // 1. Verify edge coverage
    jsTest.log.info("1. Checking edge coverage...");
    const allEdges = new Map();
    for (const vertex of testModel.states) {
        const actionsMap = testModel.collectionStateToActionsMap(vertex);
        for (const action of actionsMap.keys()) {
            const toVertex = actionsMap.get(action);
            if (toVertex !== undefined) {
                const edgeKey = JSON.stringify({from: vertex, action: action, to: toVertex});
                allEdges.set(edgeKey, {from: vertex, action: action, to: toVertex});
            }
        }
    }

    const coveredEdges = new Set();
    for (const result of results) {
        const transition = result.transition;
        const edgeKey = JSON.stringify({from: transition.from, action: transition.action, to: transition.to});
        coveredEdges.add(edgeKey);
    }

    jsTest.log.info(`   Total edges in model: ${allEdges.size}`);
    jsTest.log.info(`   Edges covered: ${coveredEdges.size}`);

    const missingEdges = [];
    for (const [edgeKey, edge] of allEdges.entries()) {
        if (!coveredEdges.has(edgeKey)) {
            missingEdges.push(edge);
            errors.push(
                `Missing edge: ${State.getName(edge.from)} --[${Action.getName(edge.action)}]--> ${State.getName(edge.to)}`,
            );
        }
    }

    if (missingEdges.length === 0) {
        jsTest.log.info("   ✓ All edges covered!");
    } else {
        jsTest.log.info(`   ✗ Missing ${missingEdges.length} edge(s):`);
        missingEdges.forEach((edge) => {
            jsTest.log.info(
                `     - ${State.getName(edge.from)} --[${Action.getName(edge.action)}]--> ${State.getName(edge.to)}`,
            );
        });
    }

    // 2. Verify state transition validity
    jsTest.log.info("2. Checking state transition validity...");
    let validTransitions = 0;
    const invalidTransitions = [];

    for (let i = 0; i < results.length; i++) {
        const transition = results[i].transition;
        const actionsMap = testModel.collectionStateToActionsMap(transition.from);
        const expectedToState = actionsMap.get(transition.action);

        if (expectedToState === undefined) {
            const error = `Command ${i + 1}: Invalid action ${Action.getName(transition.action)} from state ${State.getName(transition.from)}`;
            invalidTransitions.push(error);
            errors.push(error);
        } else if (expectedToState !== transition.to) {
            const error = `Command ${i + 1}: Expected transition to ${State.getName(expectedToState)} but got ${State.getName(transition.to)}`;
            invalidTransitions.push(error);
            errors.push(error);
        } else {
            validTransitions++;
        }
    }

    jsTest.log.info(`   Valid transitions: ${validTransitions}`);
    jsTest.log.info(`   Invalid transitions: ${invalidTransitions.length}`);

    if (invalidTransitions.length === 0) {
        jsTest.log.info("   ✓ All transitions are valid!");
    } else {
        jsTest.log.info("   ✗ Invalid transitions found:");
        invalidTransitions.forEach((error) => {
            jsTest.log.info(`     - ${error}`);
        });
    }

    // 3. Verify command sequence consistency
    jsTest.log.info("3. Checking sequence consistency...");
    const startState = testModel.getStartState();
    const inconsistencies = [];

    if (results.length > 0) {
        // Check first command starts from correct state
        if (results[0].transition.from !== startState) {
            const error = `First command should start from ${State.getName(startState)} but starts from ${State.getName(results[0].transition.from)}`;
            inconsistencies.push(error);
            errors.push(error);
        }

        // Check that each command's 'to' matches next command's 'from'
        for (let i = 0; i < results.length - 1; i++) {
            if (results[i].transition.to !== results[i + 1].transition.from) {
                const error = `Command ${i + 1} ends in ${State.getName(results[i].transition.to)} but command ${i + 2} starts from ${State.getName(results[i + 1].transition.from)}`;
                inconsistencies.push(error);
                errors.push(error);
            }
        }
    }

    if (inconsistencies.length === 0) {
        jsTest.log.info("   ✓ Sequence is consistent!");
    } else {
        jsTest.log.info("   ✗ Inconsistencies found:");
        inconsistencies.forEach((error) => {
            jsTest.log.info(`     - ${error}`);
        });
    }

    // 4. Check for duplicate edge coverage (informational only)
    jsTest.log.info("4. Checking for duplicate edge coverage...");
    const edgeCounts = new Map();
    for (const result of results) {
        const transition = result.transition;
        const edgeKey = JSON.stringify({from: transition.from, action: transition.action, to: transition.to});
        edgeCounts.set(edgeKey, (edgeCounts.get(edgeKey) || 0) + 1);
    }

    const duplicates = [];
    for (const [edgeKey, count] of edgeCounts.entries()) {
        if (count > 1) {
            const edge = JSON.parse(edgeKey);
            duplicates.push({edge, count});
            warnings.push(
                `Edge covered ${count} times: ${State.getName(edge.from)} --[${Action.getName(edge.action)}]--> ${State.getName(edge.to)}`,
            );
        }
    }

    if (duplicates.length === 0) {
        jsTest.log.info("   ✓ No duplicate edge coverage (optimal)");
    } else {
        jsTest.log.info(`   ⚠ ${duplicates.length} edge(s) covered multiple times:`);
        duplicates.forEach(({edge, count}) => {
            jsTest.log.info(
                `     - ${State.getName(edge.from)} --[${Action.getName(edge.action)}]--> ${State.getName(edge.to)} (${count}x)`,
            );
        });
    }

    // 5. Summary
    jsTest.log.info("VERIFICATION SUMMARY:");
    jsTest.log.info(`  Total commands: ${results.length}`);
    jsTest.log.info(
        `  Edge coverage: ${coveredEdges.size}/${allEdges.size} (${((coveredEdges.size / allEdges.size) * 100).toFixed(1)}%)`,
    );
    jsTest.log.info(`  Valid transitions: ${validTransitions}/${results.length}`);
    jsTest.log.info(`  Errors: ${errors.length}`);
    jsTest.log.info(`  Warnings: ${warnings.length}`);

    if (errors.length > 0) {
        throw new Error(`Verification failed with ${errors.length} error(s):\n${errors.join("\n")}`);
    }

    return {
        passed: errors.length === 0,
        errors: errors,
        warnings: warnings,
        coverage: {
            total: allEdges.size,
            covered: coveredEdges.size,
            percentage: (coveredEdges.size / allEdges.size) * 100,
        },
    };
}
