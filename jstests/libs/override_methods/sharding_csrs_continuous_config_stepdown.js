import {ContinuousStepdown} from "jstests/libs/override_methods/continuous_stepdown.js";
import "jstests/libs/override_methods/implicitly_retry_on_config_stepdowns.js";
import {kOverrideConstructor as kOverrideConstructorForRST, ReplSetTest} from "jstests/libs/replsettest.js";
import {kOverrideConstructor as kOverrideConstructorForST, ShardingTest} from "jstests/libs/shardingtest.js";

function isSlowBuildFromTestOptions() {
    const testOptions = jsTestOptions();
    return (
        testOptions.isAddressSanitizerActive ||
        testOptions.isThreadSanitizerActive ||
        testOptions.isDebug ||
        _isWindows()
    );
}

const stepdownIntervalMS = isSlowBuildFromTestOptions() ? 15 * 1000 : 8 * 1000;

const {ReplSetTestWithContinuousPrimaryStepdown, ShardingTestWithContinuousPrimaryStepdown} =
    ContinuousStepdown.configure(
        {
            configStepdown: true,
            electionTimeoutMS: 5 * 1000,
            shardStepdown: false,
            stepdownDurationSecs: 10,
            stepdownIntervalMS: stepdownIntervalMS,
        },
        {
            verbositySetting: {
                verbosity: 0,
                command: {verbosity: 1},
                network: {verbosity: 1, asio: {verbosity: 2}},
            },
        },
    );

ReplSetTest[kOverrideConstructorForRST] = ReplSetTestWithContinuousPrimaryStepdown;
ShardingTest[kOverrideConstructorForST] = class ShardingTestWithContinuousFailover extends (
    ShardingTestWithContinuousPrimaryStepdown
) {
    constructor(params) {
        super(params);
        // Set the feature on the test configuration; this will allow js tests to selectively
        // skip/alter test cases.
        TestData.runningWithConfigStepdowns = true;
        // Automatically start the continuous stepdown thread on the config server replica set.
        this.startContinuousFailover();
    }
};

// The checkUUIDsConsistentAcrossCluster() function is defined on ShardingTest's prototype, but
// ShardingTest's prototype gets reset when ShardingTest is reassigned. We reload the override
// to redefine checkUUIDsConsistentAcrossCluster() on the new ShardingTest's prototype.
await import("jstests/libs/override_methods/check_uuids_consistent_across_cluster.js");
