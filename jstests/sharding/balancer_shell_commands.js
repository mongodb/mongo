/**
 * Test shell balancer commands.
 *  sh.setBalancerState
 *  sh.getBalancerState
 *
 *  @tags: [
 *   # SERVER-54796 sh.enableAutoSplit() writes to the config server
 *   # through mongos that doesn't support retries to config server
 *   does_not_support_stepdowns,
 * ]
 */

var db;

(function() {
"use strict";
var shardingTest =
    new ShardingTest({name: "shell_commands", shards: 1, mongos: 1, other: {enableBalancer: true}});
db = shardingTest.getDB("test");

assert(sh.getBalancerState(), "Balancer should have been enabled during cluster setup");

// Test that the balancer can be disabled
sh.setBalancerState(false);
assert(!sh.getBalancerState(), "Failed to disable balancer");

sh.enableAutoSplit();
assert(!sh.getBalancerState(), "Autosplit-only should not classify the balancer as enabled");

// Test that the balancer can be re-enabled
sh.setBalancerState(true);
assert(sh.getBalancerState(), "Failed to re-enable balancer");

shardingTest.stop();
})();
