// Tests that updates can't change immutable fields (used in sharded system)
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s;
    var config = mongos.getDB("config");
    var coll = mongos.getCollection(jsTestName() + ".coll1");
    var shard0 = st.shard0;

    assert.commandWorked(config.adminCommand({enableSharding: coll.getDB() + ""}));
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0000');
    assert.commandWorked(config.adminCommand({shardCollection: "" + coll, key: {a: 1}}));

    var getDirectShardedConn = function(st, collName) {

        var shardConnWithVersion = new Mongo(st.shard0.host);

        var configConnStr = st._configDB;

        var maxChunk =
            st.s0.getCollection("config.chunks").find({ns: collName}).sort({lastmod: -1}).next();

        var ssvInitCmd = {
            setShardVersion: collName,
            authoritative: true,
            configdb: configConnStr,
            version: maxChunk.lastmod,
            shard: 'shard0000',
            versionEpoch: maxChunk.lastmodEpoch
        };

        printjson(ssvInitCmd);
        assert.commandWorked(shardConnWithVersion.getDB("admin").runCommand(ssvInitCmd));

        return shardConnWithVersion;
    };

    var shard0Coll = getDirectShardedConn(st, coll.getFullName()).getCollection(coll.getFullName());

    // No shard key
    shard0Coll.remove({});
    assert.writeError(shard0Coll.save({_id: 3}));

    // Full shard key in save
    assert.writeOK(shard0Coll.save({_id: 1, a: 1}));

    // Full shard key on replacement (basically the same as above)
    shard0Coll.remove({});
    assert.writeOK(shard0Coll.update({_id: 1}, {a: 1}, true));

    // Full shard key after $set
    shard0Coll.remove({});
    assert.writeOK(shard0Coll.update({_id: 1}, {$set: {a: 1}}, true));

    // Update existing doc (replacement), same shard key value
    assert.writeOK(shard0Coll.update({_id: 1}, {a: 1}));

    // Update existing doc ($set), same shard key value
    assert.writeOK(shard0Coll.update({_id: 1}, {$set: {a: 1}}));

    // Error due to mutating the shard key (replacement)
    assert.writeError(shard0Coll.update({_id: 1}, {b: 1}));

    // Error due to mutating the shard key ($set)
    assert.writeError(shard0Coll.update({_id: 1}, {$unset: {a: 1}}));

    // Error due to removing all the embedded fields.
    shard0Coll.remove({});

    assert.writeOK(shard0Coll.save({_id: 2, a: {c: 1, b: 1}}));

    assert.writeError(shard0Coll.update({}, {$unset: {"a.c": 1}}));
    assert.writeError(shard0Coll.update({}, {$unset: {"a.b": 1, "a.c": 1}}));

    st.stop();

})();
