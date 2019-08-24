/**
 * Verify that operations which must run on all shards, such as $currentOp and $changeStream, do not
 * crash when shards are added to or removed from the cluster mid-operation.
 *
 * @tags: [requires_sharding, requires_non_retryable_writes, catches_command_failures,
 * uses_change_streams, uses_curop_agg_stage]
 */

"use strict";

var $config = (function() {
    // The 'setup' function is run once by the parent thread after the cluster has been initialized,
    // before the worker threads have been spawned. The 'this' argument is bound as '$config.data'.
    function setup(db, collName, cluster) {
        // Obtain the list of shards present in the cluster. Used to remove and restore shards.
        this.shardList = db.getSiblingDB("config").shards.find().toArray();
        // Drop the test database. It's not needed and will complicate re-adding shards.
        assert.commandWorked(db.dropDatabase());
    }

    // Returns a random integer in the range [0, max).
    function randomInt(max) {
        return Math.floor(Math.random() * Math.floor(max));
    }

    // Helper to close a clusterwide cursor, given a command result.
    function closeClusterWideCursor(db, res) {
        if (res.ok) {
            db.adminCommand({
                killCursors: "$cmd.aggregate",
                cursors: [res.cursor.id],
            });
        }
    }

    var states = {
        runChangeStream: function(db, collName) {
            const res = db.adminCommand({
                aggregate: 1,
                pipeline: [{$changeStream: {allChangesForCluster: true}}],
                cursor: {}
            });
            closeClusterWideCursor(db, res);
        },

        runCurrentOp: function(db, collName) {
            const res = db.adminCommand({aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}});
            closeClusterWideCursor(db, res);
        },

        removeShard: function(db, collName) {
            // Make sure that only a single removeShard operation is running at any time.
            const testLocksColl = db.getSiblingDB("config").testLocks;
            if (!testLocksColl.insert({_id: "removeShard"}).nInserted) {
                return;
            }
            // Iterate until we successfully remove a shard or run out of shards.
            for (let shardIdx = 0; shardIdx < this.shardList.length; ++shardIdx) {
                const shardName = this.shardList[shardIdx]._id;
                if (db.adminCommand({removeShard: shardName}).state === "started") {
                    break;
                }
            }
            // Remove the lock document so that other threads can call removeShard.
            assert.commandWorked(testLocksColl.remove({_id: "removeShard"}));
        },

        addShard: function addShard(db, collName) {
            const shardIdx = randomInt(this.shardList.length);
            const shardEntry = this.shardList[shardIdx];
            db.adminCommand({addShard: shardEntry.host, name: shardEntry._id});
        },

        init: function(db, collName) {
            // Do nothing. This is only used to randomize the first action taken by each worker.
        }
    };

    const transitionProbabilities =
        {runChangeStream: 0.25, runCurrentOp: 0.25, removeShard: 0.25, addShard: 0.25};
    var transitions = {
        init: transitionProbabilities,
        runChangeStream: transitionProbabilities,
        runCurrentOp: transitionProbabilities,
        removeShard: transitionProbabilities,
        addShard: transitionProbabilities
    };

    // The 'teardown' function is run once by the parent thread before the cluster is destroyed, but
    // after the worker threads have been reaped. The 'this' argument is bound as '$config.data'.
    function teardown(db, collName, cluster) {
        // If any shards are draining, unset them so we don't impact subsequent tests.
        db.getSiblingDB("config").shards.update({}, {$unset: {draining: 1}}, {multi: true});
        // Ensure that all shards are present in the cluster before shutting down the ShardingTest.
        for (let shardEntry of this.shardList) {
            assert.soon(() => db.adminCommand({addShard: shardEntry.host, name: shardEntry._id}).ok,
                        `failed to add shard ${shardEntry._id} back into cluster at end of test`);
        }
    }

    return {
        threadCount: 100,
        iterations: 1000,
        startState: "init",
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown
    };
})();
