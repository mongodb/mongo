(function() {
    "use strict";
    load("jstests/libs/uuid_util.js");

    var verifyOutput = function(out, output) {
        printjson(out);
        assert.eq(out.counts.input, 51200, "input count is wrong");
        assert.eq(out.counts.emit, 51200, "emit count is wrong");
        assert.gt(out.counts.reduce, 99, "reduce count is wrong");
        assert.eq(out.counts.output, output, "output count is wrong");
    };

    var st = new ShardingTest({shards: 2, verbose: 1, mongos: 1, other: {chunkSize: 1}});

    var admin = st.s0.getDB('admin');

    assert.commandWorked(admin.runCommand({enablesharding: "mrShard"}));
    st.ensurePrimaryShard('mrShard', st.shard1.shardName);
    assert.commandWorked(
        admin.runCommand({shardcollection: "mrShard.srcSharded", key: {"_id": 1}}));

    var db = st.s0.getDB("mrShard");

    var bulk = db.srcSharded.initializeUnorderedBulkOp();
    for (var j = 0; j < 100; j++) {
        for (var i = 0; i < 512; i++) {
            bulk.insert({j: j, i: i});
        }
    }
    assert.writeOK(bulk.execute());

    function map() {
        emit(this.i, 1);
    }
    function reduce(key, values) {
        return Array.sum(values);
    }

    // sharded src sharded dst
    var suffix = "InShardedOutSharded";

    // Check that merge to an existing empty sharded collection works and creates a new UUID after
    // M/R
    st.adminCommand({shardcollection: "mrShard.mergeSharded", key: {"_id": 1}});
    var origUUID = getUUIDFromConfigCollections(st.s, "mrShard.mergeSharded");

    var out = db.srcSharded.mapReduce(map, reduce, {out: {merge: "mergeSharded", sharded: true}});
    verifyOutput(out, 512);

    var newUUID = getUUIDFromConfigCollections(st.s, "mrShard.mergeSharded");
    assert.neq(origUUID, newUUID);

    // Check that merge to an existing sharded collection has data on all shards works and that the
    // collection uses the same UUID after M/R
    db.mergeSharded.drop();
    st.adminCommand({shardcollection: "mrShard.mergeSharded", key: {"_id": 1}});
    assert.commandWorked(admin.runCommand({split: "mrShard.mergeSharded", middle: {"_id": 2000}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: "mrShard.mergeSharded", find: {"_id": 2000}, to: st.shard0.shardName}));
    assert.writeOK(st.s.getCollection("mrShard.mergeSharded").insert({_id: 1000}));
    assert.writeOK(st.s.getCollection("mrShard.mergeSharded").insert({_id: 2001}));
    origUUID = getUUIDFromConfigCollections(st.s, "mrShard.mergeSharded");

    out = db.srcSharded.mapReduce(map, reduce, {out: {merge: "mergeSharded", sharded: true}});
    verifyOutput(out, 514);

    newUUID = getUUIDFromConfigCollections(st.s, "mrShard.mergeSharded");
    assert.eq(origUUID, newUUID);

    // Check that replace to an existing sharded collection has data on all shards works and that
    // the
    // collection creates a new UUID after M/R
    db.replaceSharded.drop();
    st.adminCommand({shardcollection: "mrShard.replaceSharded", key: {"_id": 1}});
    assert.commandWorked(
        admin.runCommand({split: "mrShard.replaceSharded", middle: {"_id": 2000}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: "mrShard.replaceSharded", find: {"_id": 2000}, to: st.shard0.shardName}));
    assert.writeOK(st.s.getCollection("mrShard.replaceSharded").insert({_id: 1000}));
    assert.writeOK(st.s.getCollection("mrShard.replaceSharded").insert({_id: 2001}));
    origUUID = getUUIDFromConfigCollections(st.s, "mrShard.replaceSharded");

    out = db.srcSharded.mapReduce(map, reduce, {out: {replace: "replaceSharded", sharded: true}});
    verifyOutput(out, 512);

    newUUID = getUUIDFromConfigCollections(st.s, "mrShard.replaceSharded");
    assert.neq(origUUID, newUUID);

    // Check that reduce to an existing unsharded collection fails when `sharded: true`
    assert.commandWorked(db.runCommand({create: "reduceUnsharded"}));
    assert.commandFailed(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {reduce: "reduceUnsharded", sharded: true}
    }));

    assert.commandWorked(db.reduceUnsharded.insert({x: 1}));
    assert.commandFailed(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {reduce: "reduceUnsharded", sharded: true}
    }));

    // Check that replace to an existing unsharded collection works when `sharded: true`
    assert.commandWorked(db.runCommand({create: "replaceUnsharded"}));
    assert.commandWorked(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {replace: "replaceUnsharded", sharded: true}
    }));

    assert.commandWorked(db.replaceUnsharded.insert({x: 1}));
    assert.commandWorked(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {replace: "replaceUnsharded", sharded: true}
    }));

    st.stop();

})();
