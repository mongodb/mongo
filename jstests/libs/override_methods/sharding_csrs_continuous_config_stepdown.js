import {ContinuousStepdown} from "jstests/libs/override_methods/continuous_stepdown.js";

const {ReplSetTestWithContinuousPrimaryStepdown, ShardingTestWithContinuousPrimaryStepdown} =
    ContinuousStepdown.configure({
        configStepdown: true,
        electionTimeoutMS: 5 * 1000,
        shardStepdown: false,
        stepdownDurationSecs: 10,
        stepdownIntervalMS: 8 * 1000,
    },
                                 {
                                     verbositySetting: {
                                         verbosity: 0,
                                         command: {verbosity: 1},
                                         network: {verbosity: 1, asio: {verbosity: 2}}
                                     }
                                 });

globalThis.ReplSetTest = ReplSetTestWithContinuousPrimaryStepdown;
globalThis.ShardingTest =
    class ShardingTestWithContinuousFailover extends ShardingTestWithContinuousPrimaryStepdown {
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
