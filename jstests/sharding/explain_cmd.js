// Tests for the mongos explain command.
(function() {
    'use strict';

    // Create a cluster with 3 shards.
    var st = new ShardingTest({shards: 2});

    var db = st.s.getDB("test");
    var explain;

    // Setup a collection that will be sharded. The shard key will be 'a'. There's also an index on
    // 'b'.
    var collSharded = db.getCollection("mongos_explain_cmd");
    collSharded.drop();
    collSharded.ensureIndex({a: 1});
    collSharded.ensureIndex({b: 1});

    // Enable sharding.
    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), 'shard0001');
    db.adminCommand({shardCollection: collSharded.getFullName(), key: {a: 1}});

    // Pre-split the collection to ensure that both shards have chunks. Explicitly
    // move chunks since the balancer is disabled.
    for (var i = 1; i <= 2; i++) {
        assert.commandWorked(db.adminCommand({split: collSharded.getFullName(), middle: {a: i}}));

        var shardName = "shard000" + (i - 1);
        printjson(
            db.adminCommand({moveChunk: collSharded.getFullName(), find: {a: i}, to: shardName}));
    }

    // Put data on each shard.
    for (var i = 0; i < 3; i++) {
        collSharded.insert({_id: i, a: i, b: 1});
    }

    st.printShardingStatus();

    // Test a scatter-gather count command.
    assert.eq(3, collSharded.count({b: 1}));

    // Explain the scatter-gather count.
    explain = db.runCommand(
        {explain: {count: collSharded.getName(), query: {b: 1}}, verbosity: "allPlansExecution"});

    // Validate some basic properties of the result.
    printjson(explain);
    assert.commandWorked(explain);
    assert("queryPlanner" in explain);
    assert("executionStats" in explain);
    assert.eq(2, explain.queryPlanner.winningPlan.shards.length);
    assert.eq(2, explain.executionStats.executionStages.shards.length);

    // An explain of a command that doesn't exist should fail gracefully.
    explain = db.runCommand({
        explain: {nonexistent: collSharded.getName(), query: {b: 1}},
        verbosity: "allPlansExecution"
    });
    printjson(explain);
    assert.commandFailed(explain);

    // -------

    // Setup a collection that is not sharded.
    var collUnsharded = db.getCollection("mongos_explain_cmd_unsharded");
    collUnsharded.drop();
    collUnsharded.ensureIndex({a: 1});
    collUnsharded.ensureIndex({b: 1});

    for (var i = 0; i < 3; i++) {
        collUnsharded.insert({_id: i, a: i, b: 1});
    }
    assert.eq(3, collUnsharded.count({b: 1}));

    explain = db.runCommand({
        explain: {
            group: {
                ns: collUnsharded.getName(),
                key: "a",
                cond: "b",
                $reduce: function(curr, result) {},
                initial: {}
            }
        },
        verbosity: "allPlansExecution"
    });

    // Basic validation: a group command can only be passed through to an unsharded collection,
    // so we should confirm that the mongos stage is always SINGLE_SHARD.
    printjson(explain);
    assert.commandWorked(explain);
    assert("queryPlanner" in explain);
    assert("executionStats" in explain);
    assert.eq("SINGLE_SHARD", explain.queryPlanner.winningPlan.stage);

    // The same group should fail over the sharded collection, because group is only supported
    // if it is passed through to an unsharded collection.
    explain = db.runCommand({
        explain: {
            group: {
                ns: collSharded.getName(),
                key: "a",
                cond: "b",
                $reduce: function(curr, result) {},
                initial: {}
            }
        },
        verbosity: "allPlansExecution"
    });
    printjson(explain);
    assert.commandFailed(explain);

    // -------

    // Explain a delete operation and verify that it hits all shards without the shard key
    explain = db.runCommand({
        explain: {delete: collSharded.getName(), deletes: [{q: {b: 1}, limit: 0}]},
        verbosity: "allPlansExecution"
    });
    assert.commandWorked(explain, tojson(explain));
    assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_WRITE");
    assert.eq(explain.queryPlanner.winningPlan.shards.length, 2);
    assert.eq(explain.queryPlanner.winningPlan.shards[0].winningPlan.stage, "DELETE");
    assert.eq(explain.queryPlanner.winningPlan.shards[1].winningPlan.stage, "DELETE");
    // Check that the deletes didn't actually happen.
    assert.eq(3, collSharded.count({b: 1}));

    // Explain a delete operation and verify that it hits only one shard with the shard key
    explain = db.runCommand({
        explain: {delete: collSharded.getName(), deletes: [{q: {a: 1}, limit: 0}]},
        verbosity: "allPlansExecution"
    });
    assert.commandWorked(explain, tojson(explain));
    assert.eq(explain.queryPlanner.winningPlan.shards.length, 1);
    // Check that the deletes didn't actually happen.
    assert.eq(3, collSharded.count({b: 1}));

    // Check that we fail gracefully if we try to do an explain of a write batch that has more
    // than one operation in it.
    explain = db.runCommand({
        explain: {
            delete: collSharded.getName(),
            deletes: [{q: {a: 1}, limit: 1}, {q: {a: 2}, limit: 1}]
        },
        verbosity: "allPlansExecution"
    });
    assert.commandFailed(explain, tojson(explain));

    // Explain a multi upsert operation and verify that it hits all shards
    explain = db.runCommand({
        explain:
            {update: collSharded.getName(), updates: [{q: {}, u: {$set: {b: 10}}, multi: true}]},
        verbosity: "allPlansExecution"
    });
    assert.commandWorked(explain, tojson(explain));
    assert.eq(explain.queryPlanner.winningPlan.shards.length, 2);
    assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_WRITE");
    assert.eq(explain.queryPlanner.winningPlan.shards.length, 2);
    assert.eq(explain.queryPlanner.winningPlan.shards[0].winningPlan.stage, "UPDATE");
    assert.eq(explain.queryPlanner.winningPlan.shards[1].winningPlan.stage, "UPDATE");
    // Check that the update didn't actually happen.
    assert.eq(0, collSharded.count({b: 10}));

    // Explain an upsert operation and verify that it hits only a single shard
    explain = db.runCommand({
        explain: {update: collSharded.getName(), updates: [{q: {a: 10}, u: {a: 10}, upsert: true}]},
        verbosity: "allPlansExecution"
    });
    assert.commandWorked(explain, tojson(explain));
    assert.eq(explain.queryPlanner.winningPlan.shards.length, 1);
    // Check that the upsert didn't actually happen.
    assert.eq(0, collSharded.count({a: 10}));

    // Explain an upsert operation which cannot be targeted, ensure an error is thrown
    explain = db.runCommand({
        explain: {update: collSharded.getName(), updates: [{q: {b: 10}, u: {b: 10}, upsert: true}]},
        verbosity: "allPlansExecution"
    });
    assert.commandFailed(explain, tojson(explain));

    st.stop();
})();
