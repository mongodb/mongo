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

    var assertCollectionNotOnShard = function(db, coll) {
        var listCollsRes = db.runCommand({listCollections: 1, filter: {name: coll}});
        assert.commandWorked(listCollsRes);
        assert.neq(undefined, listCollsRes.cursor);
        assert.neq(undefined, listCollsRes.cursor.firstBatch);
        assert.eq(0, listCollsRes.cursor.firstBatch.length);
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
    st.adminCommand({shardcollection: "mrShard.outSharded", key: {"_id": 1}});
    var origUUID = getUUIDFromConfigCollections(st.s, "mrShard.outSharded");
    var out = db.srcSharded.mapReduce(map, reduce, {out: {merge: "outSharded", sharded: true}});
    verifyOutput(out, 512);
    var newUUID = getUUIDFromConfigCollections(st.s, "mrShard.outSharded");
    assert.neq(origUUID, newUUID);

    // Shard1 is the primary shard and only one chunk should have been written, so the chunk with
    // the new UUID should have been written to it.
    assert.eq(newUUID, getUUIDFromListCollections(st.shard1.getDB("mrShard"), "outSharded"));

    // Shard0 should not have any chunks from the output collection because all shards should have
    // returned an empty split point list in the first phase of the mapReduce, since the reduced
    // data size is far less than the chunk size setting of 1MB.
    assertCollectionNotOnShard(st.shard0.getDB("mrShard"), "outSharded");

    // Check that merge to an existing sharded collection that has data on all shards works and that
    // the collection uses the same UUID after M/R
    assert.commandWorked(admin.runCommand({split: "mrShard.outSharded", middle: {"_id": 2000}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: "mrShard.outSharded", find: {"_id": 2000}, to: st.shard0.shardName}));
    assert.writeOK(st.s.getCollection("mrShard.outSharded").insert({_id: 1000}));
    assert.writeOK(st.s.getCollection("mrShard.outSharded").insert({_id: 2001}));
    origUUID = getUUIDFromConfigCollections(st.s, "mrShard.outSharded");

    out = db.srcSharded.mapReduce(map, reduce, {out: {merge: "outSharded", sharded: true}});
    verifyOutput(out, 514);

    newUUID = getUUIDFromConfigCollections(st.s, "mrShard.outSharded");
    assert.eq(origUUID, newUUID);
    assert.eq(newUUID, getUUIDFromListCollections(st.shard0.getDB("mrShard"), "outSharded"));
    assert.eq(newUUID, getUUIDFromListCollections(st.shard1.getDB("mrShard"), "outSharded"));

    // Check that replace to an existing sharded collection has data on all shards works and that
    // the collection creates a new UUID after M/R.
    origUUID = getUUIDFromConfigCollections(st.s, "mrShard.outSharded");
    out = db.srcSharded.mapReduce(map, reduce, {out: {replace: "outSharded", sharded: true}});
    verifyOutput(out, 512);

    newUUID = getUUIDFromConfigCollections(st.s, "mrShard.outSharded");
    assert.neq(origUUID, newUUID);

    // Shard1 is the primary shard and only one chunk should have been written, so the chunk with
    // the new UUID should have been written to it.
    assert.eq(newUUID, getUUIDFromListCollections(st.shard1.getDB("mrShard"), "outSharded"));

    // Shard0 should not have any chunks from the output collection because all shards should have
    // returned an empty split point list in the first phase of the mapReduce, since the reduced
    // data size is far less than the chunk size setting of 1MB.
    assertCollectionNotOnShard(st.shard0.getDB("mrShard"), "outSharded");

    // Check that reduce to an existing unsharded collection fails when `sharded: true`.
    assert.commandWorked(db.runCommand({create: "reduceUnsharded"}));
    assert.commandFailed(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {reduce: "reduceUnsharded", sharded: true}
    }));

    db.reduceUnsharded.insert({x: 1});
    assert.commandFailed(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {reduce: "reduceUnsharded", sharded: true}
    }));

    // Check that replace to an existing unsharded collection works when `sharded: true`.
    assert.commandWorked(db.runCommand({create: "replaceUnsharded"}));
    origUUID = getUUIDFromListCollections(st.s.getDB("mrShard"), "replaceUnsharded");

    assert.commandWorked(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {replace: "replaceUnsharded", sharded: true}
    }));

    newUUID = getUUIDFromConfigCollections(st.s, "mrShard.replaceUnsharded");
    assert.neq(origUUID, newUUID);
    assert.eq(newUUID, getUUIDFromListCollections(st.shard1.getDB("mrShard"), "replaceUnsharded"));

    db.replaceUnsharded.insert({x: 1});
    origUUID = getUUIDFromListCollections(st.s.getDB("mrShard"), "replaceUnsharded");

    assert.commandWorked(db.runCommand({
        mapReduce: "srcSharded",
        map: map,
        reduce: reduce,
        out: {replace: "replaceUnsharded", sharded: true}
    }));

    newUUID = getUUIDFromConfigCollections(st.s, "mrShard.replaceUnsharded");
    assert.neq(origUUID, newUUID);
    assert.eq(newUUID, getUUIDFromListCollections(st.shard1.getDB("mrShard"), "replaceUnsharded"));

    st.stop();

})();