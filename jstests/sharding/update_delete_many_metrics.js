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
    const shardedColl = testDB.shardedColl;
    const unshardedColl = testDB.unshardedColl;

    assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

    // Shard shardedColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
    st.shardColl(shardedColl, {x: 1}, {x: 0}, {x: 1});

    // Insert one document on each shard.
    assert.commandWorked(shardedColl.insert({x: 1, _id: 1}));
    assert.commandWorked(shardedColl.insert({x: 2, _id: 2}));
    assert.commandWorked(shardedColl.insert({x: -1, _id: -1}));
    assert.commandWorked(shardedColl.insert({x: -2, _id: -2}));
    assert.eq(4, shardedColl.find().itcount());

    assert.commandWorked(unshardedColl.insert({x: 1, _id: 1}));
    assert.commandWorked(unshardedColl.insert({x: -11, _id: 0}));
    assert.eq(2, unshardedColl.find().itcount());

    let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for initial values.
    assert.eq(0, mongosServerStatus.metrics.query.updateManyCount);
    assert.eq(0, mongosServerStatus.metrics.query.deleteManyCount);

    assert.commandWorked(unshardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));
    assert.eq(1, unshardedColl.find({a: 2}).count());

    assert.commandWorked(shardedColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));
    assert.eq(1, shardedColl.find({a: 2}).count());

    // 3 update with multi:true calls.
    assert.commandWorked(unshardedColl.update({}, {$set: {a: 3}}, {multi: true}));
    assert.eq(2, unshardedColl.find({a: 3}).count());
    assert.commandWorked(shardedColl.update({}, {$set: {a: 3}}, {multi: true}));
    assert.eq(0, shardedColl.find({a: 2}).count());
    assert.eq(4, shardedColl.find({a: 3}).count());
    assert.commandWorked(shardedColl.update({}, {$set: {a: 4}}, {multi: true}));
    assert.eq(4, shardedColl.find({a: 4}).count());

    // 2 updateMany calls.
    assert.commandWorked(shardedColl.updateMany({}, {$set: {array: 'string', doc: 'string'}}));
    assert.commandWorked(unshardedColl.updateMany({}, {$set: {array: 'string', doc: 'string'}}));

    // batch update: 2 more, so 7 updates in total
    var request = {
        update: shardedColl.getName(),
        updates: [{q: {}, u: {$set: {c: 3}}, multi: true}, {q: {}, u: {$set: {a: 5}}, multi: true}],
        writeConcern: {w: 1},
        ordered: false
    };
    shardedColl.runCommand(request);
    assert.eq(4, shardedColl.find({a: 5}).count());

    // Use deleteMany to delete one of the documents.
    const result = shardedColl.deleteMany({_id: 1});
    assert.commandWorked(result);
    assert.eq(1, result.deletedCount);
    assert.eq(3, shardedColl.find().itcount());
    // Next call will not increase count.
    assert.commandWorked(shardedColl.deleteOne({_id: 1}));

    // Use deleteMany to delete one document in the unsharded collection.
    assert.commandWorked(unshardedColl.deleteMany({_id: 1}));
    // Next call will not increase count.
    assert.commandWorked(unshardedColl.deleteOne({_id: 1}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});
    // Verification for metrics.
    assert.eq(7, mongosServerStatus.metrics.query.updateManyCount);
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

    // Insert documents
    assert.commandWorked(testColl.insert({x: 1, _id: 1}));
    assert.commandWorked(testColl.insert({x: 2, _id: 2}));
    assert.commandWorked(testColl.insert({x: -1, _id: -1}));
    assert.commandWorked(testColl.insert({x: -2, _id: -2}));
    assert.eq(4, testColl.find().itcount());

    let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for initial values.
    assert.eq(0, mongosServerStatus.metrics.query.updateManyCount);
    assert.eq(0, mongosServerStatus.metrics.query.deleteManyCount);
    assert.eq(0, mongosServerStatus.metrics.query.updateDeleteManyDocumentsMaxCount);
    assert.eq(0, mongosServerStatus.metrics.query.updateDeleteManyDurationMaxMs);
    assert.eq(0, mongosServerStatus.metrics.query.updateDeleteManyDocumentsTotalCount);
    assert.eq(0, mongosServerStatus.metrics.query.updateDeleteManyDurationTotalMs);

    assert.commandWorked(testColl.update({_id: 1}, {$set: {a: 2}}, {multi: false}));
    assert.eq(1, testColl.find({a: 2}).count());
    // 2 update with multi:true calls.
    assert.commandWorked(testColl.update({}, {$set: {a: 3}}, {multi: true}));
    assert.eq(0, testColl.find({a: 2}).count());
    assert.eq(4, testColl.find({a: 3}).count());
    assert.commandWorked(testColl.update({}, {$set: {a: 4}}, {multi: true}));
    assert.eq(4, testColl.find({a: 4}).count());

    // 1 updateMany call.
    assert.commandWorked(testColl.updateMany({}, {$set: {array: 'string', doc: 'string'}}));

    // batch update: 2 more, so 5 updates in total
    var request = {
        update: testColl.getName(),
        updates: [{q: {}, u: {$set: {c: 3}}, multi: true}, {q: {}, u: {$set: {a: 5}}, multi: true}],
        writeConcern: {w: 1},
        ordered: false
    };
    testColl.runCommand(request);
    assert.eq(4, testColl.find({a: 5}).count());

    // Use deleteMany to delete two of the documents.
    const result = testColl.deleteMany({_id: {$lt: 0}});
    assert.commandWorked(result);
    assert.eq(2, result.deletedCount);
    assert.eq(2, testColl.find().itcount());
    // Next call will not increase count.
    assert.commandWorked(testColl.deleteOne({_id: 1}));

    mongosServerStatus = testDB.adminCommand({serverStatus: 1});

    // Verification for final metric values.
    assert.eq(5, mongosServerStatus.metrics.query.updateManyCount);
    assert.eq(1, mongosServerStatus.metrics.query.deleteManyCount);
    assert.eq(4, mongosServerStatus.metrics.query.updateDeleteManyDocumentsMaxCount);
    assert(mongosServerStatus.metrics.query.hasOwnProperty("updateDeleteManyDurationMaxMs"));
    assert.lte(0, mongosServerStatus.metrics.query.updateDeleteManyDurationMaxMs);
    assert.eq(22, mongosServerStatus.metrics.query.updateDeleteManyDocumentsTotalCount);
    assert(mongosServerStatus.metrics.query.hasOwnProperty("updateDeleteManyDurationTotalMs"));
    assert.lte(0, mongosServerStatus.metrics.query.updateDeleteManyDurationTotalMs);

    rst.stopSet();
}
})();
