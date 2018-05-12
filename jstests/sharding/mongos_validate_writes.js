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
    st.ensurePrimaryShard(coll.getDB().getName(), st.shard1.shardName);
    coll.ensureIndex({a: 1});

    // Shard the collection on {a: 1} and move one chunk to another shard. Updates need to be across
    // two shards to trigger an error, otherwise they are versioned and will succeed after raising
    // a StaleConfigException.
    st.shardColl(coll, {a: 1}, {a: 0}, {a: 1}, coll.getDB(), true);

    // Let the stale mongos see the collection state
    staleCollA.findOne();
    staleCollB.findOne();

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({b: 1});
    st.shardColl(coll, {b: 1}, {b: 0}, {b: 1}, coll.getDB(), true);

    // Make sure that we can successfully insert, even though we have stale state
    assert.writeOK(staleCollA.insert({b: "b"}));

    // Make sure we unsuccessfully insert with old info
    assert.writeError(staleCollB.insert({a: "a"}));

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({c: 1});
    st.shardColl(coll, {c: 1}, {c: 0}, {c: 1}, coll.getDB(), true);

    // Make sure we can successfully upsert, even though we have stale state
    assert.writeOK(staleCollA.update({c: "c"}, {c: "c"}, true));

    // Make sure we unsuccessfully upsert with old info
    assert.writeError(staleCollB.update({b: "b"}, {b: "b"}, true));

    // Change the collection sharding state
    coll.drop();
    coll.ensureIndex({d: 1});
    st.shardColl(coll, {d: 1}, {d: 0}, {d: 1}, coll.getDB(), true);

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
    // Deletes need to be across two shards to trigger an error.
    st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);
    st.shardColl(coll, {e: 1}, {e: 0}, {e: 1}, coll.getDB(), true);

    // Make sure we can successfully remove, even though we have stale state
    assert.writeOK(coll.insert({e: "e"}));

    assert.writeOK(staleCollA.remove({e: "e"}, true));
    assert.eq(null, staleCollA.findOne());

    // Make sure we unsuccessfully remove with old info
    assert.writeError(staleCollB.remove({d: "d"}, true));

    st.stop();
})();
