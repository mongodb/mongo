/**
 * Verify that shard key appears in oplog records
 *
 * The only records that need to be checked are delete and update records, but updates happen on
 * various paths that must all be checked.
 */

(function() {
    "use strict";

    var st = new ShardingTest({name: 'test', shards: {rs0: {nodes: 1}}});
    var db = st.s.getDB('test');

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

    // 'test.un' is left unsharded.
    assert.commandWorked(db.adminCommand({shardcollection: 'test.byId', key: {_id: 1}}));
    assert.commandWorked(db.adminCommand({shardcollection: 'test.byX', key: {x: 1}}));
    assert.commandWorked(db.adminCommand({shardcollection: 'test.byXId', key: {x: 1, _id: 1}}));
    assert.commandWorked(db.adminCommand({shardcollection: 'test.byIdX', key: {_id: 1, x: 1}}));

    assert.writeOK(db.un.insert({_id: 10, x: 50, y: 60}));
    assert.writeOK(db.un.insert({_id: 30, x: 70, y: 80}));

    assert.writeOK(db.byId.insert({_id: 11, x: 51, y: 61}));
    assert.writeOK(db.byId.insert({_id: 31, x: 71, y: 81}));

    assert.writeOK(db.byX.insert({_id: 12, x: 52, y: 62}));
    assert.writeOK(db.byX.insert({_id: 32, x: 72, y: 82}));

    assert.writeOK(db.byXId.insert({_id: 13, x: 53, y: 63}));
    assert.writeOK(db.byXId.insert({_id: 33, x: 73, y: 83}));

    assert.writeOK(db.byIdX.insert({_id: 14, x: 54, y: 64}));
    assert.writeOK(db.byIdX.insert({_id: 34, x: 74, y: 84}));

    var oplog = st.rs0.getPrimary().getDB('local').oplog.rs;

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test update command on 'un'");

    assert.writeOK(db.un.update({_id: 10, x: 50}, {$set: {y: 70}}));  // in place
    assert.writeOK(db.un.update({_id: 30, x: 70}, {y: 75}));          // replacement

    // unsharded, only _id appears in o2:

    var a = oplog.findOne({ns: 'test.un', op: 'u', 'o2._id': 10});
    assert.eq(a.o2, {_id: 10});

    var b = oplog.findOne({ns: 'test.un', op: 'u', 'o2._id': 30});
    assert.eq(b.o2, {_id: 30});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test update command on 'byId'");

    assert.writeOK(db.byId.update({_id: 11}, {$set: {y: 71}}));  // in place
    assert.writeOK(db.byId.update({_id: 31}, {x: 71, y: 76}));   // replacement

    // sharded by {_id: 1}: only _id appears in o2:

    a = oplog.findOne({ns: 'test.byId', op: 'u', 'o2._id': 11});
    assert.eq(a.o2, {_id: 11});

    b = oplog.findOne({ns: 'test.byId', op: 'u', 'o2._id': 31});
    assert.eq(b.o2, {_id: 31});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test update command on 'byX'");

    assert.writeOK(db.byX.update({x: 52}, {$set: {y: 72}}));  // in place
    assert.writeOK(db.byX.update({x: 72}, {x: 72, y: 77}));   // replacement

    // sharded by {x: 1}: x appears in o2, followed by _id:

    a = oplog.findOne({ns: 'test.byX', op: 'u', 'o2._id': 12});
    assert.eq(a.o2, {x: 52, _id: 12});

    b = oplog.findOne({ns: 'test.byX', op: 'u', 'o2._id': 32});
    assert.eq(b.o2, {x: 72, _id: 32});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test update command on 'byXId'");

    assert.writeOK(db.byXId.update({_id: 13, x: 53}, {$set: {y: 73}}));  // in place
    assert.writeOK(db.byXId.update({_id: 33, x: 73}, {x: 73, y: 78}));   // replacement

    // sharded by {x: 1, _id: 1}: x appears in o2, followed by _id:

    a = oplog.findOne({ns: 'test.byXId', op: 'u', 'o2._id': 13});
    assert.eq(a.o2, {x: 53, _id: 13});

    b = oplog.findOne({ns: 'test.byXId', op: 'u', 'o2._id': 33});
    assert.eq(b.o2, {x: 73, _id: 33});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test update command on 'byIdX'");

    assert.writeOK(db.byIdX.update({_id: 14, x: 54}, {$set: {y: 74}}));  // in place
    assert.writeOK(db.byIdX.update({_id: 34, x: 74}, {x: 74, y: 79}));   // replacement

    // sharded by {_id: 1, x: 1}: _id appears in o2, followed by x:

    a = oplog.findOne({ns: 'test.byIdX', op: 'u', 'o2._id': 14});
    assert.eq(a.o2, {_id: 14, x: 54});

    b = oplog.findOne({ns: 'test.byIdX', op: 'u', 'o2._id': 34});
    assert.eq(b.o2, {_id: 34, x: 74});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test remove command: 'un'");

    assert.writeOK(db.un.remove({_id: 10}));
    assert.writeOK(db.un.remove({_id: 30}));

    a = oplog.findOne({ns: 'test.un', op: 'd', 'o._id': 10});
    assert.eq(a.o, {_id: 10});
    b = oplog.findOne({ns: 'test.un', op: 'd', 'o._id': 30});
    assert.eq(b.o, {_id: 30});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test remove command: 'byX'");

    assert.writeOK(db.byX.remove({_id: 12}));
    assert.writeOK(db.byX.remove({_id: 32}));

    a = oplog.findOne({ns: 'test.byX', op: 'd', 'o._id': 12});
    assert.eq(a.o, {x: 52, _id: 12});
    b = oplog.findOne({ns: 'test.byX', op: 'd', 'o._id': 32});
    assert.eq(b.o, {x: 72, _id: 32});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test remove command: 'byXId'");

    assert.writeOK(db.byXId.remove({_id: 13}));
    assert.writeOK(db.byXId.remove({_id: 33}));

    a = oplog.findOne({ns: 'test.byXId', op: 'd', 'o._id': 13});
    assert.eq(a.o, {x: 53, _id: 13});
    b = oplog.findOne({ns: 'test.byXId', op: 'd', 'o._id': 33});
    assert.eq(b.o, {x: 73, _id: 33});

    ////////////////////////////////////////////////////////////////////////
    jsTest.log("Test remove command: 'byIdX'");

    assert.writeOK(db.byIdX.remove({_id: 14}));
    assert.writeOK(db.byIdX.remove({_id: 34}));

    a = oplog.findOne({ns: 'test.byIdX', op: 'd', 'o._id': 14});
    assert.eq(a.o, {_id: 14, x: 54});
    b = oplog.findOne({ns: 'test.byIdX', op: 'd', 'o._id': 34});
    assert.eq(b.o, {_id: 34, x: 74});

    st.stop();
})();
