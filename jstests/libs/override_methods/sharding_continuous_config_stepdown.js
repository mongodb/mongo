(function() {
"use strict";

load("jstests/libs/override_methods/continuous_stepdown.js");

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
                                     network: {verbosity: 1, asio: {verbosity: 2}},
                                     tracking: {verbosity: 0}
                                 }
                             });

const originalShardingTest = ShardingTest;
ShardingTest = function() {
    originalShardingTest.apply(this, arguments);

    // Automatically start the continuous stepdown thread on the config server replica set.
    this.startContinuousFailover();
};

// The checkUUIDsConsistentAcrossCluster() function is defined on ShardingTest's prototype, but
// ShardingTest's prototype gets reset when ShardingTest is reassigned. We reload the override
// to redefine checkUUIDsConsistentAcrossCluster() on the new ShardingTest's prototype.
load('jstests/libs/override_methods/check_uuids_consistent_across_cluster.js');
})();
