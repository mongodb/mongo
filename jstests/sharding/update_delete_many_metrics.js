/**
 * Tests for the 'metrics.query' section of the mongos and mongod serverStatus response verifying
 * counters for updateMany and deleteMany
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
    const mongodConns = [];
    st.rs0.nodes.forEach(node => mongodConns.push(node));
    st.rs1.nodes.forEach(node => mongodConns.push(node));

    const testDB = st.s.getDB("test");
    const testColl = testDB.coll;
    const unshardedColl = testDB.unsharded;

    assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

    // Shard testColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
    st.shardColl(testColl, {x: 1}, {x: 0}, {x: 1});

    // Insert one document on each shard.
    assert.commandWorked(testColl.insert({x: 1, _id: 1}));
    assert.commandWorked(testColl.insert({x: -1, _id: 0}));

    assert.eq(2, testColl.countDocuments({}));
    assert.commandWorked(unshardedColl.insert({x: 1, _id: 1}));
    assert.eq(1, unshardedColl.countDocuments({}));

    let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for 'updateManyCount' metric.
    assert.eq(0, mongosServerStatus.metrics.query.updateManyCount);
    // Verification for 'deleteManyCount' metric.
    assert.eq(0, mongosServerStatus.metrics.query.deleteManyCount);

    assert.commandWorked(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));
    assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));
    // 3 update with multi:true calls.
    assert.commandWorked(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: true}));
    assert.commandWorked(testColl.update({}, {$set: {a: 3}}, {multi: true}));
    assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 2}}, {multi: true}));

    // 2 updateMany calls.
    assert.commandWorked(testColl.updateMany({}, {$set: {array: 'string', doc: 'string'}}));
    assert.commandWorked(unshardedColl.updateMany({}, {$set: {array: 'string', doc: 'string'}}));

    // Use deleteMany to delete one of the documents.
    const result = testColl.deleteMany({_id: 1});
    assert.commandWorked(result);
    assert.eq(1, result.deletedCount);
    assert.eq(1, testColl.countDocuments({}));
    // Next call will not increase count.
    assert.commandWorked(testColl.deleteOne({_id: 1}));

    // Use deleteMany to delete one document in the unsharded collection.
    assert.commandWorked(unshardedColl.deleteMany({_id: 1}));
    // Next call will not increase count.
    assert.commandWorked(unshardedColl.deleteOne({_id: 1}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for 'updateManyCount' metric.
    assert.eq(5, mongosServerStatus.metrics.query.updateManyCount);
    // Verification for 'deleteManyCount' metric.
    assert.eq(2, mongosServerStatus.metrics.query.deleteManyCount);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB("test");
    const testColl = testDB.coll;
    const unshardedColl = testDB.unsharded;

    // Insert one document on each shard.
    assert.commandWorked(testColl.insert({x: 1, _id: 1}));
    assert.commandWorked(testColl.insert({x: -1, _id: 0}));
    assert.eq(2, testColl.countDocuments({}));

    let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for 'updateManyCount' metric.
    assert.eq(0, mongosServerStatus.metrics.query.updateManyCount);
    // Verification for 'deleteManyCount' metric.
    assert.eq(0, mongosServerStatus.metrics.query.deleteManyCount);

    assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));
    // 3 update with multi:true calls.
    assert.commandWorked(testColl.update({}, {$set: {a: 3}}, {multi: true}));
    assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 2}}, {multi: true}));

    // 2 updateMany call.
    assert.commandWorked(testColl.updateMany({}, {$set: {array: 'string', doc: 'string'}}));

    // Use deleteMany to delete one of the documents.
    const result = testColl.deleteMany({_id: 1});
    assert.commandWorked(result);
    assert.eq(1, result.deletedCount);
    assert.eq(1, testColl.countDocuments({}));
    // Next call will not increase count.
    assert.commandWorked(testColl.deleteOne({_id: 1}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for 'updateManyCount' metric.
    assert.eq(3, mongosServerStatus.metrics.query.updateManyCount);
    // Verification for 'deleteManyCount' metric.
    assert.eq(1, mongosServerStatus.metrics.query.deleteManyCount);

    rst.stopSet();
}
})();
