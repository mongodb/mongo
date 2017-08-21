(function() {
    "use strict";

    load("jstests/libs/override_methods/continuous_stepdown.js");
    load("jstests/libs/override_methods/mongos_manual_intervention_actions.js");

    ContinuousStepdown.configure({
        configStepdown: true,
        electionTimeoutMS: 5 * 1000,
        shardStepdown: false,
        stepdownDurationSecs: 10,
        stepdownIntervalMS: 8 * 1000,
    });

    const originalShardingTest = ShardingTest;
    ShardingTest = function() {
        originalShardingTest.apply(this, arguments);

        // Automatically start the continuous stepdown thread on the config server replica set.
        this.startContinuousFailover();
    };
})();
