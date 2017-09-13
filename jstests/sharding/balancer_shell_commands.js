/**
 * Test shell balancer commands.
 *  sh.setBalancerState
 *  sh.getBalancerState
 */

var db;

(function() {
    "use strict";
    var shardingTest = new ShardingTest(
        {name: "shell_commands", shards: 1, mongos: 1, other: {enableBalancer: true}});
    db = shardingTest.getDB("test");

    assert(sh.getBalancerState(), "Balancer should have been enabled during cluster setup");

    // Test that the balancer can be disabled
    sh.setBalancerState(false);
    assert(!sh.getBalancerState(), "Failed to disable balancer");

    // Test that the balancer can be re-enabled
    sh.setBalancerState(true);
    assert(sh.getBalancerState(), "Failed to re-enable balancer");

    shardingTest.stop();
})();
