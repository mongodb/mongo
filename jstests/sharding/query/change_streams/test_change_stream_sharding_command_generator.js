/**
 * Generates all possible change stream test scenarios via combinatorial testing.
 * Explores state transitions for database and collection operations in a sharded cluster.
 */
import {CollectionTestModel} from "jstests/libs/util/change_stream/change_stream_collection_test_model.js";
import {ShardingCommandGenerator} from "jstests/libs/util/change_stream/change_stream_sharding_command_generator.js";
import {State} from "jstests/libs/util/change_stream/change_stream_state.js";
import {Action} from "jstests/libs/util/change_stream/change_stream_action.js";
import {describe, it} from "jstests/libs/mochalite.js";

describe("ShardingCommandGenerator", function () {
    it("should generate identical command sequences for the same seed", () => {
        const testSeed = 42;
        const model1 = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const model2 = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);

        const gen1 = new ShardingCommandGenerator(testSeed);
        const gen2 = new ShardingCommandGenerator(testSeed);

        const commands1 = gen1.generateCommands(model1);
        const commands2 = gen2.generateCommands(model2);

        jsTest.log.info(`Generated ${commands1.length} commands with seed ${testSeed}`);
        assert.eq(commands1.length, commands2.length, "Same seed should produce same number of commands");

        for (let i = 0; i < commands1.length; i++) {
            assert.eq(commands1[i].from, commands2[i].from, `Command ${i}: from state mismatch`);
            assert.eq(commands1[i].action, commands2[i].action, `Command ${i}: action mismatch`);
            assert.eq(commands1[i].to, commands2[i].to, `Command ${i}: to state mismatch`);
        }
    });

    it("should generate valid command sequence covering all edges", () => {
        const testModel = new CollectionTestModel().setStartState(State.DATABASE_ABSENT);
        const testSeed = new Date().getTime();
        const generator = new ShardingCommandGenerator(testSeed);

        jsTest.log.info(`RANDOM SEED: ${generator.getSeed()}`);
        jsTest.log.info(`Start State: ${State.getName(testModel.getStartState())}`);
        jsTest.log.info("To reproduce: new ShardingCommandGenerator(" + generator.getSeed() + ")");

        const commands = generator.generateCommands(testModel);
        jsTest.log.info(`Total commands: ${commands.length}`);

        // Log command sequence
        jsTest.log.info("\nCommand sequence:");
        commands.forEach((cmd, index) => {
            jsTest.log.info(
                `  ${index + 1}. ${State.getName(cmd.from)} --[${Action.getName(cmd.action)}]--> ${State.getName(cmd.to)}`,
            );
        });

        // Run verification
        verifyCommands(testModel, commands);
    });
});

/**
 * Verifies that the generated commands are correct and complete.
 */
function verifyCommands(testModel, commands) {
    const errors = [];
    const warnings = [];

    // 1. Verify edge coverage - check that all edges in the model are covered
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
    for (const cmd of commands) {
        const edgeKey = JSON.stringify({from: cmd.from, action: cmd.action, to: cmd.to});
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

    // 2. Verify state transitions are valid according to the model
    jsTest.log.info("2. Checking state transition validity...");
    let validTransitions = 0;
    const invalidTransitions = [];

    for (let i = 0; i < commands.length; i++) {
        const cmd = commands[i];
        const actionsMap = testModel.collectionStateToActionsMap(cmd.from);
        const expectedToState = actionsMap.get(cmd.action);

        if (expectedToState === undefined) {
            const error = `Command ${i + 1}: Invalid action ${Action.getName(cmd.action)} from state ${State.getName(cmd.from)}`;
            invalidTransitions.push(error);
            errors.push(error);
        } else if (expectedToState !== cmd.to) {
            const error = `Command ${i + 1}: Expected transition to ${State.getName(expectedToState)} but got ${State.getName(cmd.to)}`;
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

    if (commands.length > 0) {
        // Check first command starts from correct state
        if (commands[0].from !== startState) {
            const error = `First command should start from ${State.getName(startState)} but starts from ${State.getName(commands[0].from)}`;
            inconsistencies.push(error);
            errors.push(error);
        }

        // Check that each command's 'to' matches next command's 'from'
        for (let i = 0; i < commands.length - 1; i++) {
            if (commands[i].to !== commands[i + 1].from) {
                const error = `Command ${i + 1} ends in ${State.getName(commands[i].to)} but command ${i + 2} starts from ${State.getName(commands[i + 1].from)}`;
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

    // 4. Check for duplicate edges (informational)
    jsTest.log.info("4. Checking for duplicate edge coverage...");
    const edgeCounts = new Map();
    for (const cmd of commands) {
        const edgeKey = JSON.stringify({from: cmd.from, action: cmd.action, to: cmd.to});
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
    jsTest.log.info(`  Total commands: ${commands.length}`);
    jsTest.log.info(
        `  Edge coverage: ${coveredEdges.size}/${allEdges.size} (${((coveredEdges.size / allEdges.size) * 100).toFixed(1)}%)`,
    );
    jsTest.log.info(`  Valid transitions: ${validTransitions}/${commands.length}`);
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
