/**
 * Test shell balancer commands.
 *  sh.setBalancerState
 *  sh.getBalancerLockDetails
 *  sh.getBalancerState
 */

var db;

(function() {
    "use strict";
    var shardingTest = new ShardingTest(
        {name: "shell_commands", shards: 1, mongos: 1, other: {enableBalancer: true}});
    db = shardingTest.getDB("test");

    assert(sh.getBalancerState(), "Balancer should have been enabled during cluster setup");

    // Test that the balancer can be disabled and the balancer lock is still locked.
    sh.setBalancerState(false);
    var lock = sh.getBalancerLockDetails();
    assert.eq("balancer", lock._id);
    assert.eq(2, lock.state);
    assert(!sh.getBalancerState(), "Failed to disable balancer");

    // Test that the balancer can be enabled and the balancer lock is still locked.
    sh.setBalancerState(true);
    lock = sh.getBalancerLockDetails();
    assert.eq("balancer", lock._id);
    assert.eq(2, lock.state);
    assert(sh.getBalancerState(), "Failed to enable balancer");

    shardingTest.stop();
})();
