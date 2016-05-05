//
// Tests that mongos validating writes when stale does not DOS config servers
//
// Note that this is *unsafe* with broadcast removes and updates
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 3, other: {shardOptions: {verbose: 2}}});

    var mongos = st.s0;
    var staleMongosA = st.s1;
    var staleMongosB = st.s2;

    var admin = mongos.getDB("admin");
    var config = mongos.getDB("config");
    var coll = mongos.getCollection("foo.bar");
    var staleCollA = staleMongosA.getCollection(coll + "");
    var staleCollB = staleMongosB.getCollection(coll + "");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
    coll.ensureIndex({a: 1});
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {a: 1}}));

    // Let the stale mongos see the collection state
    staleCollA.findOne();
    staleCollB.findOne();

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({b: 1});
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {b: 1}}));

    // Make sure that we can successfully insert, even though we have stale state
    assert.writeOK(staleCollA.insert({b: "b"}));

    // Make sure we unsuccessfully insert with old info
    assert.writeError(staleCollB.insert({a: "a"}));

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({c: 1});
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {c: 1}}));

    // Make sure we can successfully upsert, even though we have stale state
    assert.writeOK(staleCollA.update({c: "c"}, {c: "c"}, true));

    // Make sure we unsuccessfully upsert with old info
    assert.writeError(staleCollB.update({b: "b"}, {b: "b"}, true));

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({d: 1});
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {d: 1}}));

    // Make sure we can successfully update, even though we have stale state
    assert.writeOK(coll.insert({d: "d"}));

    assert.writeOK(staleCollA.update({d: "d"}, {$set: {x: "x"}}, false, false));
    assert.eq(staleCollA.findOne().x, "x");

    // Make sure we unsuccessfully update with old info
    assert.writeError(staleCollB.update({c: "c"}, {$set: {x: "y"}}, false, false));
    assert.eq(staleCollB.findOne().x, "x");

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({e: 1});
    // Deletes need to be across two shards to trigger an error - this is probably an exceptional
    // case
    assert.commandWorked(admin.runCommand({movePrimary: coll.getDB() + "", to: "shard0000"}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {e: 1}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {e: 0}}));
    assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {e: 0}, to: "shard0001"}));

    // Make sure we can successfully remove, even though we have stale state
    assert.writeOK(coll.insert({e: "e"}));

    assert.writeOK(staleCollA.remove({e: "e"}, true));
    assert.eq(null, staleCollA.findOne());

    // Make sure we unsuccessfully remove with old info
    assert.writeError(staleCollB.remove({d: "d"}, true));

    st.stop();
})();
