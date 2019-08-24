/**
 * Verify that operations which must run on all shards, such as $currentOp and $changeStream, do not
 * crash when shards are added to the cluster mid-operation, or when config.shards is dropped.
 *
 * This test inherits from 'sharded_clusterwide_ops_with_add_remove_shards.js' but is kept separate
 * from it, because (1) we may remove the ability to write to config.shards in the future, at which
 * point this test can simply be removed; and (2) running a single FSM test with both removeShard
 * and config.shards.remove({}) can cause the former to hang indefinitely while waiting for the
 * removed shard to drain.
 *
 * @tags: [requires_sharding, requires_non_retryable_writes, catches_command_failures,
 * uses_change_streams, uses_curop_agg_stage]
 */

"use strict";

// For base $config setup.
const baseDir = 'jstests/concurrency/fsm_workloads_add_remove_shards/';
load(baseDir + 'clusterwide_ops_with_add_remove_shards.js');

// After loading the base file, $config has been populated with states and transitions. We simply
// overwrite 'states.removeShard' such that it instantly wipes all shards from the cluster rather
// than removing a single shard via the removeShard command. This is the only way to test that
// mongoS is resilient to the sudden absence of shards in the middle of an operation, as the
// removeShard command is not permitted to remove the last existing shard in the cluster.
$config.states.removeShard = function(db, collName) {
    assert.commandWorked(db.getSiblingDB("config").shards.remove({}));
};
