/**
 * The admin and config databases are special in that their primary shard is always the config
 * shard. This test verifies that dropping unsharded collections in the admin or config database
 * applies the drop on (only) the config shard.
 */

(function() {
"use strict";

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

// Disable the logical session cache refresh on config servers because it auto-recreates the
// config.system.sessions collection if config.system.sessions is dropped, and this test manually
// controls config.system.sessions.
const st = new ShardingTest({
    shards: 1,
    other: {configOptions: {setParameter: {"disableLogicalSessionCacheRefresh": true}}}
});

(() => {
    jsTest.log(
        "Dropping a user collection in the admin database only executes on the config shard");

    const [collName, ns] = getNewNs("admin");

    // Insert a document through mongos; it should be inserted on the config shard.
    assert.commandWorked(st.s.getDB("admin").getCollection(collName).insert(
        {_id: 1}, {writeConcern: {w: "majority"}}));
    assert.eq({_id: 1}, st.configRS.getPrimary().getDB("admin").getCollection(collName).findOne());
    assert.eq(null, st.rs0.getPrimary().getDB("admin").getCollection(collName).findOne());

    // Insert a document directly on the shard.
    assert.commandWorked(st.shard0.getDB("admin").getCollection(collName).insert(
        {_id: 2}, {writeConcern: {w: "majority"}}));
    assert.eq({_id: 2}, st.rs0.getPrimary().getDB("admin").getCollection(collName).findOne());

    // Drop the collection through mongos; it should be executed only on the config shard.
    st.s.getDB("admin").getCollection(collName).drop();
    assert.eq(null, st.configRS.getPrimary().getDB("admin").getCollection(collName).findOne());
    assert.eq({_id: 2}, st.rs0.getPrimary().getDB("admin").getCollection(collName).findOne());
})();

(() => {
    jsTest.log(
        "Dropping a user collection in the config database only executes on the config shard");

    const [collName, ns] = getNewNs("config");

    // Insert a document through mongos; it should be inserted on the config shard.
    assert.commandWorked(st.s.getDB("config").getCollection(collName).insert(
        {_id: 1}, {writeConcern: {w: "majority"}}));
    assert.eq({_id: 1}, st.configRS.getPrimary().getDB("config").getCollection(collName).findOne());
    assert.eq(null, st.rs0.getPrimary().getDB("config").getCollection(collName).findOne());

    // Insert a document directly on the shard.
    assert.commandWorked(st.shard0.getDB("config").getCollection(collName).insert(
        {_id: 2}, {writeConcern: {w: "majority"}}));
    assert.eq({_id: 2}, st.rs0.getPrimary().getDB("config").getCollection(collName).findOne());

    // Drop the collection through mongos; it should be executed only on the config shard.
    st.s.getDB("config").getCollection(collName).drop();
    assert.eq(null, st.configRS.getPrimary().getDB("config").getCollection(collName).findOne());
    assert.eq({_id: 2}, st.rs0.getPrimary().getDB("config").getCollection(collName).findOne());
})();

(() => {
    jsTest.log("Dropping config.system.sessions always only executes on non-config shards");

    // Wait for the config.system.sessions collection to be created and sharded.
    assert.soon(function() {
        return st.s.getDB("config").collections.findOne({_id: "config.system.sessions"}) != null;
    });
    assert.eq(0,
              st.configRS.getPrimary()
                  .getDB("config")
                  .getCollectionInfos({name: "system.sessions"})
                  .length);
    assert.eq(1, st.shard0.getDB("config").getCollectionInfos({name: "system.sessions"}).length);

    // Create a separate config.system.sessions directly on the config shard.
    assert.commandWorked(st.configRS.getPrimary().getDB("config").system.sessions.insert(
        {_id: 1}, {writeConcern: {w: "majority"}}));
    assert.eq(1,
              st.configRS.getPrimary()
                  .getDB("config")
                  .getCollectionInfos({name: "system.sessions"})
                  .length);

    // Drop the config.system.sessions collection through mongos; the drop should be executed only
    // on the regular shard.
    st.s.getDB("config").system.sessions.drop();
    assert.eq(0, st.shard0.getDB("config").getCollectionInfos({name: "system.sessions"}).length);
    assert.eq(1,
              st.configRS.getPrimary()
                  .getDB("config")
                  .getCollectionInfos({name: "system.sessions"})
                  .length);

    // Re-create a config.system.sessions collection directly on the regular shard.
    assert.commandWorked(st.shard0.getDB("config").system.sessions.insert(
        {_id: 2}, {writeConcern: {w: "majority"}}));
    assert.eq(1, st.shard0.getDB("config").getCollectionInfos({name: "system.sessions"}).length);

    // Drop the config.system.sessions collection through mongos; the drop should be executed only
    // on the regular shard even though config.system.sessions is marked as dropped: true.
    assert.eq(true,
              st.s.getDB("config").collections.findOne({_id: "config.system.sessions"}).dropped);
    st.s.getDB("config").system.sessions.drop();
    assert.eq(0, st.shard0.getDB("config").getCollectionInfos({name: "system.sessions"}).length);
    assert.eq(1,
              st.configRS.getPrimary()
                  .getDB("config")
                  .getCollectionInfos({name: "system.sessions"})
                  .length);

    // Re-create a config.system.sessions collection directly on the regular shard.
    assert.commandWorked(st.shard0.getDB("config").system.sessions.insert(
        {_id: 2}, {writeConcern: {w: "majority"}}));
    assert.eq(1, st.shard0.getDB("config").getCollectionInfos({name: "system.sessions"}).length);

    // Remove the entry for config.system.sessions from config.collections.
    assert.commandWorked(st.s.getDB("config").collections.remove({_id: "config.system.sessions"},
                                                                 {writeConcern: {w: "majority"}}));

    // Drop the config.system.sessions collection through mongos; the drop should be executed only
    // on the regular shard even though config.system.sessions is not in config.collections.
    st.s.getDB("config").system.sessions.drop();
    assert.eq(0, st.shard0.getDB("config").getCollectionInfos({name: "system.sessions"}).length);
    assert.eq(1,
              st.configRS.getPrimary()
                  .getDB("config")
                  .getCollectionInfos({name: "system.sessions"})
                  .length);
})();

st.stop();
})();
