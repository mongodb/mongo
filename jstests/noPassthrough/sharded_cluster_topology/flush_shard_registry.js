/**
 * Tests the _flushShardRegistry command.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 3}});

const mongos = st.s;
const shard0 = st.shard0;
const shard1 = st.shard1;
const configsvr = st.configRS.getPrimary();

// Test 1: Run command on shard server
jsTest.log.info("Test 1: Running _flushShardRegistry on shard server");
{
    let result = shard0.adminCommand({_flushShardRegistry: 1});
    jsTest.log.debug("Shard0 refresh result: " + tojson(result));
    assert.commandWorked(result);
    assert(result.hasOwnProperty("cachedTimeBefore"), "Response should contain cachedTimeBefore field");
    assert(result.hasOwnProperty("cachedTimeAfter"), "Response should contain cachedTimeAfter field");
    assert(
        result.hasOwnProperty("forceReloadIncrementBefore"),
        "Response should contain forceReloadIncrementBefore field",
    );
    assert(
        result.hasOwnProperty("forceReloadIncrementAfter"),
        "Response should contain forceReloadIncrementAfter field",
    );
    assert.lt(
        result.forceReloadIncrementBefore,
        result.forceReloadIncrementAfter,
        "Force reload increment should be greater than after",
    );
}

// Test 2: Run on different secondaries of the same replica set of shard0
jsTest.log.info("Test 2: Running _flushShardRegistry on different secondaries");
{
    let secondaries = st.rs0.getSecondaries();
    for (let secondary of secondaries) {
        let result = secondary.adminCommand({_flushShardRegistry: 1});
        jsTest.log.debug("Secondary refresh result: " + tojson(result));
        assert.commandWorked(result);
        assert.lt(
            result.forceReloadIncrementBefore,
            result.forceReloadIncrementAfter,
            "Force reload increment should be greater than after",
        );
    }
}

// Test 3: Run on config server
jsTest.log.info("Test 3: Running _flushShardRegistry on config server");
{
    let result = configsvr.adminCommand({_flushShardRegistry: 1});
    jsTest.log.debug("Config server refresh result: " + tojson(result));
    assert.commandWorked(result);
    assert.lt(
        result.forceReloadIncrementBefore,
        result.forceReloadIncrementAfter,
        "Force reload increment should be greater than after",
    );
}

// Test 4: Run command on router
jsTest.log.info("Test 4: Running _flushShardRegistry on router");
{
    let result = mongos.adminCommand({_flushShardRegistry: 1});
    jsTest.log.debug("Shard0 refresh result: " + tojson(result));
    assert.commandWorked(result);
    assert(result.hasOwnProperty("cachedTimeBefore"), "Response should contain cachedTimeBefore field");
    assert(result.hasOwnProperty("cachedTimeAfter"), "Response should contain cachedTimeAfter field");
    assert(
        result.hasOwnProperty("forceReloadIncrementBefore"),
        "Response should contain forceReloadIncrementBefore field",
    );
    assert(
        result.hasOwnProperty("forceReloadIncrementAfter"),
        "Response should contain forceReloadIncrementAfter field",
    );
    assert.lt(
        result.forceReloadIncrementBefore,
        result.forceReloadIncrementAfter,
        "Force reload increment should be greater than after",
    );
}

// Test 5: Command fails on non-admin database
jsTest.log.info("Test 5: Command should fail on non-admin database");
{
    let result = shard0.getDB("test").runCommand({_flushShardRegistry: 1});
    assert.commandFailed(result);
}

// Test 6: Command works after adding a new shard
jsTest.log.info("Test 6: Verify refresh picks up newly added shard");
{
    // Add a new shard
    const newShard = new ReplSetTest({name: "newShard", nodes: 1});
    newShard.startSet({shardsvr: ""});
    newShard.initiate();

    assert.commandWorked(mongos.adminCommand({addShard: newShard.getURL(), name: "shard2"}));

    // Recover on router
    let result = mongos.adminCommand({_flushShardRegistry: 1});
    jsTest.log.debug("After adding shard: " + tojson(result));
    assert.commandWorked(result);
    assert.lt(
        result.forceReloadIncrementBefore,
        result.forceReloadIncrementAfter,
        "Force reload increment should be greater than after",
    );

    // Cleanup
    assert.commandWorked(mongos.adminCommand({removeShard: "shard2"}));
    assert.soon(() => {
        let removeResult = mongos.adminCommand({removeShard: "shard2"});
        return removeResult.state === "completed";
    });

    newShard.stopSet();
}

// Test 6: Command is idempotent
jsTest.log.info("Test 6: Command is idempotent - multiple calls should succeed");
{
    for (let i = 0; i < 3; i++) {
        let result = mongos.adminCommand({_flushShardRegistry: 1});
        assert.commandWorked(result);
        assert.lt(
            result.forceReloadIncrementBefore,
            result.forceReloadIncrementAfter,
            "Force reload increment should be greater than after",
        );
    }
}

// Test 7: Create sharded and untracked collections, query before and after recovery
jsTest.log.info("Test 7: Query sharded and untracked collections before and after recovery");
{
    const testDB = mongos.getDB("testDB7");
    const shardedColl = testDB.shardedColl;
    const untrackedColl = testDB.untrackedColl;

    // Enable sharding on the database
    assert.commandWorked(mongos.adminCommand({enableSharding: "testDB7"}));

    // Create and shard the first collection
    assert.commandWorked(mongos.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

    // Insert data into sharded collection
    let shardedBulk = shardedColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 50; i++) {
        shardedBulk.insert({_id: i, type: "sharded", value: i * 2});
    }
    assert.commandWorked(shardedBulk.execute());

    // Create untracked collection
    let untrackedBulk = untrackedColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 50; i++) {
        untrackedBulk.insert({_id: i, type: "untracked", value: i * 3});
    }
    assert.commandWorked(untrackedBulk.execute());

    // Query both collections before recovery
    jsTest.log.info("Querying collections before recovery");
    let shardedCountBefore = shardedColl.count();
    let untrackedCountBefore = untrackedColl.count();
    let shardedSampleBefore = shardedColl.findOne({_id: 10});
    let untrackedSampleBefore = untrackedColl.findOne({_id: 10});

    assert.eq(shardedCountBefore, 50, "Sharded collection should have 50 documents before recovery");
    assert.eq(untrackedCountBefore, 50, "Untracked collection should have 50 documents before recovery");
    assert.eq(shardedSampleBefore.value, 20, "Sharded collection sample data should be correct");
    assert.eq(untrackedSampleBefore.value, 30, "Untracked collection sample data should be correct");

    // Run recovery
    jsTest.log.info("Running _flushShardRegistry");
    let result = mongos.adminCommand({_flushShardRegistry: 1});
    assert.commandWorked(result);
    assert.lt(
        result.forceReloadIncrementBefore,
        result.forceReloadIncrementAfter,
        "Force reload increment should be greater than after",
    );

    // Query both collections after recovery
    jsTest.log.info("Querying collections after recovery");
    let shardedCountAfter = shardedColl.count();
    let untrackedCountAfter = untrackedColl.count();
    let shardedSampleAfter = shardedColl.findOne({_id: 10});
    let untrackedSampleAfter = untrackedColl.findOne({_id: 10});

    // Verify data is unchanged
    assert.eq(shardedCountAfter, 50, "Sharded collection should still have 50 documents after recovery");
    assert.eq(untrackedCountAfter, 50, "Untracked collection should still have 50 documents after recovery");
    assert.eq(shardedSampleAfter.value, 20, "Sharded collection sample data should be unchanged");
    assert.eq(untrackedSampleAfter.value, 30, "Untracked collection sample data should be unchanged");

    jsTest.log.info("Both sharded and untracked collections accessible before and after recovery");

    // Cleanup
    testDB.dropDatabase();
}

st.stop();

jsTest.log.info("All tests passed!");
