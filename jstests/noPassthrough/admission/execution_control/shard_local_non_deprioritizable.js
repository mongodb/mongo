/**
 * Tests that specific shard-local internal commands are marked as NonDeprioritizable.
 *
 * This ensures that system-critical operations issued by shards/mongos to shards
 * are not throttled by admission control deprioritization.
 *
 * The test uses serverStatus metrics to verify that operations are marked as NonDeprioritizable.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_timeseries,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

const kUntrackedTimeseriesColl = "testUntrackedTimeserieColl";
const kTrackedTimeseriesColl = "testTrackedTimeserieColl";

/**
 * Configuration for shard-local internal commands that should be marked as NonDeprioritizable. Each entry defines a test case.
 */
const kShardLocalNonDeprioritizableCommands = [
    {
        name: "Critical section enter",
        description: "Critical section acquisitions for shard participants.",
        db: "admin",
        command: (shardConn) => ({
            _shardsvrParticipantBlock: "participant_block_enter",
            blockType: "ReadsAndWrites",
            test: "non_deprioritizable_enter_critical_section",
            writeConcern: {w: "majority"},
        }),
    },
    {
        name: "Critical section exit",
        description: "Critical section releases for shard participants.",
        db: "admin",
        command: (shardConn) => ({
            _shardsvrParticipantBlock: "participant_block_exit",
            blockType: "Unblock",
            test: "non_deprioritizable_exit_critical_section",
            writeConcern: {w: "majority"},
        }),
    },
    // TODO (SERVER-116499): Remove the following four _shardsvrTimeseriesUpgradeDowngradeCommit test cases once 9.0 becomes last LTS.
    {
        name: "Untracked timeseries upgrade commit",
        description: "Untracked timeseries upgrade commit for shard.",
        db: "testDB",
        command: (shardConn) => ({
            _shardsvrTimeseriesUpgradeDowngradeCommit: kUntrackedTimeseriesColl,
            mode: "upgradeToViewless",
            isTracked: false,
            databasePrimaryShardId: shardConn.shardName,
            lsid: {id: UUID()},
            txnNumber: NumberLong(1),
            writeConcern: {w: "majority"},
        }),
    },
    {
        name: "Untracked timeseries downgrade commit",
        description: "Untracked timeseries downgrade commit for shard.",
        db: "testDB",
        command: (shardConn) => ({
            _shardsvrTimeseriesUpgradeDowngradeCommit: kUntrackedTimeseriesColl,
            mode: "downgradeToLegacy",
            isTracked: false,
            databasePrimaryShardId: shardConn.shardName,
            lsid: {id: UUID()},
            txnNumber: NumberLong(1),
            writeConcern: {w: "majority"},
        }),
    },
    {
        name: "Tracked timeseries upgrade commit",
        description: "Tracked timeseries upgrade commit for shard.",
        db: "testDB",
        command: (shardConn) => ({
            _shardsvrTimeseriesUpgradeDowngradeCommit: kTrackedTimeseriesColl,
            mode: "upgradeToViewless",
            isTracked: true,
            databasePrimaryShardId: shardConn.shardName,
            lsid: {id: UUID()},
            txnNumber: NumberLong(1),
            writeConcern: {w: "majority"},
        }),
    },
    {
        name: "Tracked timeseries downgrade commit",
        description: "Tracked timeseries downgrade commit for shard.",
        db: "testDB",
        command: (shardConn) => ({
            _shardsvrTimeseriesUpgradeDowngradeCommit: kTrackedTimeseriesColl,
            mode: "downgradeToLegacy",
            isTracked: true,
            databasePrimaryShardId: shardConn.shardName,
            lsid: {id: UUID()},
            txnNumber: NumberLong(1),
            writeConcern: {w: "majority"},
        }),
    },
    {
        name: "Local drop collection",
        description: "Local drop collection operations for shard participants.",
        db: "admin",
        command: (shardConn) => ({
            _shardsvrDropCollectionParticipant: "testDB.testColl",
            lsid: {id: UUID()},
            txnNumber: NumberLong(1),
            writeConcern: {w: "majority"},
        }),
    },
    // Add more commands here as they get NonDeprioritizable protection:
    // {
    //     name: "Example command",
    //     description: "Description of what this protects",
    //     db: "admin",
    //     command: (shardConn) => ({exampleCmd: 1}),
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

    assert.commandWorked(shardConn.getDB(testCase.db).runCommand(testCase.command(shardConn)));

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

    assert.commandWorked(st.shard0.getDB("testDB")["testColl"].insert({x: 1}));

    const createViewlessTimeseriesEnabled = FeatureFlagUtil.isPresentAndEnabled(
        st.s.getDB("admin"),
        "CreateViewlessTimeseriesCollections",
    );

    /// TODO (SERVER-116499): Remove the following timeseries collections once 9.0 becomes last LTS.
    // Create both untracked and tracked timeseries collections to be used in the timeseries upgrade/downgrade commit test cases.
    const testDB = st.s.getDB("testDB");
    assert.commandWorked(testDB.createCollection(kUntrackedTimeseriesColl, {timeseries: {timeField: "t"}}));
    assert.commandWorked(
        testDB.runCommand({
            createUnsplittableCollection: kTrackedTimeseriesColl,
            dataShard: st.shard0.shardName,
            timeseries: {timeField: "t"},
        }),
    );

    // Iterate over the test cases and verify that each command is marked as NonDeprioritizable.
    try {
        for (const testCase of kShardLocalNonDeprioritizableCommands) {
            // TODO (SERVER-116499): Remove the following conditional checks for timeseries collections once 9.0 becomes last LTS.
            if (
                createViewlessTimeseriesEnabled &&
                (testCase.name === "Untracked timeseries downgrade commit" ||
                    testCase.name === "Tracked timeseries downgrade commit")
            ) {
                jsTest.log.info(
                    "Skipping 'Untracked/Tracked timeseries downgrade commit' test case because CreateViewlessTimeseriesCollections is enabled",
                );
                continue;
            } else if (
                !createViewlessTimeseriesEnabled &&
                (testCase.name === "Untracked timeseries upgrade commit" ||
                    testCase.name === "Tracked timeseries upgrade commit")
            ) {
                jsTest.log.info(
                    "Skipping 'Untracked/Tracked timeseries upgrade commit' test case because CreateViewlessTimeseriesCollections is disabled",
                );
                continue;
            }
            testCommandIsNonDeprioritizable(st.shard0, testCase);
        }
    } finally {
        st.stop();
    }
}

// Run all shard-local internal tests
runShardLocalTests();
jsTest.log.info("All shard-local internal NonDeprioritizable commands tests passed");
