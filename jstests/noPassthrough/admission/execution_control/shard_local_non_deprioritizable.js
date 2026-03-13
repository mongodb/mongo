/**
 * Tests that specific shard-local internal commands are marked as NonDeprioritizable.
 *
 * This ensures that system-critical operations issued by shards/mongos to shards
 * are not throttled by admission control deprioritization.
 *
 * The test uses serverStatus metrics to verify that operations are marked as NonDeprioritizable.
 *
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

/**
 * Configuration for shard-local internal commands that should be marked as NonDeprioritizable. Each entry defines a test case.
 */
const kShardLocalNonDeprioritizableCommands = [
    {
        name: "Critical section enter",
        description: "Critical section acquisitions for shard participants.",
        db: "admin",
        command: {
            _shardsvrParticipantBlock: "participant_block_enter",
            blockType: "ReadsAndWrites",
            test: "non_deprioritizable_enter_critical_section",
            writeConcern: {w: "majority"},
        },
    },
    {
        name: "Critical section exit",
        description: "Critical section releases for shard participants.",
        db: "admin",
        command: {
            _shardsvrParticipantBlock: "participant_block_exit",
            blockType: "Unblock",
            test: "non_deprioritizable_exit_critical_section",
            writeConcern: {w: "majority"},
        },
    },
    // Add more commands here as they get NonDeprioritizable protection:
    // {
    //     name: "Example command",
    //     description: "Description of what this protects",
    //     db: "admin",
    //     command: {exampleCmd: 1},
    // },
];

/**
 * Tests that a command is marked as NonDeprioritizable.
 */
function testCommandIsNonDeprioritizable(shardConn, testCase) {
    jsTest.log.info(`Testing: ${testCase.name}`);
    jsTest.log.info(`  Description: ${testCase.description}`);
    const shardPrimary = shardConn.rs.getPrimary();

    // Get baseline count
    const beforeCount = getTotalMarkedNonDeprioritizableCount(shardPrimary);
    jsTest.log.info(`  NonDeprioritizable count before: ${beforeCount}`);

    // Issue the command
    assert.commandWorked(shardConn.adminCommand(testCase.command));

    // Check that the counter increased
    const afterCount = getTotalMarkedNonDeprioritizableCount(shardPrimary);
    jsTest.log.info(`  NonDeprioritizable count after: ${afterCount}`);

    assert.gt(afterCount, beforeCount, `Command '${testCase.name}' should increment NonDeprioritizable counter`);

    jsTest.log.info(`  PASSED: ${testCase.name}`);
}

/* Main tests runner.
 */
function runShardLocalTests() {
    const st = new ShardingTest({shards: 1, mongos: 1});

    try {
        for (const testCase of kShardLocalNonDeprioritizableCommands) {
            testCommandIsNonDeprioritizable(st.shard0, testCase);
        }
    } finally {
        st.stop();
    }
}

// Run all shard-local internal tests
runShardLocalTests();
jsTest.log.info("All shard-local internal NonDeprioritizable commands tests passed");
