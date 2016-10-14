load('jstests/libs/write_concern_util.js');

/**
 * This commands tests that moveChunk gives a proper response when the writeConcern cannot be met.
 * The test creates a sharded cluster with shards and config servers of different sizes to see how
 * moveChunk commands react as the writeConcern changes. It first sees that a writeConcern too high
 * for the config server replicaset succeeds because moveChunk passes w: majority to config servers.
 * It then passes a writeConcern too high for the to shard and sees that it fails. It then passes
 * a writeConcern too high for the from shard and sees that that fails. moveChunk does not yield
 * a writeConcernError. It should simply fail when the writeConcern is not met on the shards.
 */

(function() {
    "use strict";
    var st = new ShardingTest({
        shards: {
            rs0: {nodes: 3, settings: {chainingAllowed: false}},
            rs1: {nodes: 5, settings: {chainingAllowed: false}}
        },
        mongos: 1,
        config: 1,
        configReplSetTestOptions: {settings: {chainingAllowed: false}}
    });

    var mongos = st.s;
    var dbName = "move-chunk-wc-test";
    var db = mongos.getDB(dbName);
    var collName = 'leaves';
    var coll = db[collName];
    var numberDoc = 20;
    var s0 = st.shard0.shardName;
    var s1 = st.shard1.shardName;

    coll.ensureIndex({x: 1}, {unique: true});
    st.ensurePrimaryShard(db.toString(), s0);
    st.shardColl(collName, {x: 1}, {x: numberDoc / 2}, {x: numberDoc / 2}, db.toString(), true);

    for (var i = 0; i < numberDoc; i++) {
        coll.insert({x: i});
    }
    assert.eq(coll.count(), numberDoc);

    // Checks that each shard has the expected number of chunks.
    function checkChunkCount(s0Count, s1Count) {
        var chunkCounts = st.chunkCounts(collName, db.toString());
        assert.eq(chunkCounts[s0], s0Count);
        assert.eq(chunkCounts[s1], s1Count);
    }
    checkChunkCount(1, 1);

    var req = {
        moveChunk: coll.toString(),
        find: {x: numberDoc / 2},
        to: s0,
        _secondaryThrottle: true,
        _waitForDelete: true
    };

    req.writeConcern = {w: 1, wtimeout: 30000};
    jsTest.log("Testing " + tojson(req));
    var res = db.adminCommand(req);
    assert.commandWorked(res);
    assert(!res.writeConcernError, 'moveChunk had writeConcernError: ' + tojson(res));
    checkChunkCount(2, 0);

    // This should pass because w: majority is always passed to config servers.
    req.writeConcern = {w: 2, wtimeout: 30000};
    jsTest.log("Testing " + tojson(req));
    req.to = s1;
    res = db.adminCommand(req);
    assert.commandWorked(res);
    assert(!res.writeConcernError, 'moveChunk had writeConcernError: ' + tojson(res));
    checkChunkCount(1, 1);

    // This should fail because the writeConcern cannot be satisfied on the to shard.
    req.writeConcern = {w: 4, wtimeout: 3000};
    jsTest.log("Testing " + tojson(req));
    req.to = s0;
    res = db.adminCommand(req);
    assert.commandFailed(res);
    assert(!res.writeConcernError, 'moveChunk had writeConcernError: ' + tojson(res));
    checkChunkCount(1, 1);

    // This should fail because the writeConcern cannot be satisfied on the from shard.
    req.writeConcern = {w: 6, wtimeout: 3000};
    jsTest.log("Testing " + tojson(req));
    req.to = s0;
    res = db.adminCommand(req);
    assert.commandFailed(res);
    assert(!res.writeConcernError, 'moveChunk had writeConcernError: ' + tojson(res));
    checkChunkCount(1, 1);

    // This should fail because the writeConcern is invalid and cannot be satisfied anywhere.
    req.writeConcern = {w: "invalid", wtimeout: 3000};
    jsTest.log("Testing " + tojson(req));
    req.to = s0;
    res = db.adminCommand(req);
    assert.commandFailed(res);
    assert(!res.writeConcernError, 'moveChunk had writeConcernError: ' + tojson(res));
    checkChunkCount(1, 1);
})();
