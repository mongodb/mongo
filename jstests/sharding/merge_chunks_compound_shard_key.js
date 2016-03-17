//
// Tests that merging chunks via mongos works/doesn't work with different chunk configurations
// with a compound shard key.
//

(function() {
    'use strict';

    var getShardVersion = function() {
        var res = st.shard0.adminCommand({getShardVersion: coll + ""});
        assert.commandWorked(res);
        var version = res.global;
        assert(version);
        return version;
    };

    // Merge two neighboring chunks and check post conditions.
    var checkMergeWorked = function(lowerBound, upperBound) {
        var oldVersion = getShardVersion();
        var numChunksBefore = chunks.find().itcount();

        assert.commandWorked(
            admin.runCommand({mergeChunks: coll + "", bounds: [lowerBound, upperBound]}));

        assert.eq(numChunksBefore - 1, chunks.find().itcount());
        assert.eq(1, chunks.find({min: lowerBound, max: upperBound}).itcount());

        var newVersion = getShardVersion();
        assert.eq(newVersion.t, oldVersion.t);
        assert.gt(newVersion.i, oldVersion.i);
    };

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s;
    var admin = mongos.getDB("admin");
    var shards = mongos.getCollection("config.shards").find().toArray();
    var chunks = mongos.getCollection("config.chunks");
    var coll = mongos.getCollection("foo.bar");

    jsTest.log("Create a sharded collection with a compound shard key.");
    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: st.shard0.shardName}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {x: 1, y: 1}}));

    // Chunks after splits:
    // (MinKey, { x: 0, y: 1 })
    // ({ x: 0, y: 1 }, { x: 1, y: 0 })
    // ({ x: 1, y: 0 }, { x: 2, y: 0 })
    // ({ x: 2, y: 0 }, { x: 2, y: 1 })
    // ({ x: 2, y: 1 }, MaxKey)
    jsTest.log("Create chunks.");
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 0, y: 1}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 1, y: 0}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 2, y: 0}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 2, y: 1}}));

    jsTest.log("Insert some data into each of the chunk ranges.");
    assert.writeOK(coll.insert({x: -1, y: 2}));
    assert.writeOK(coll.insert({x: 0, y: 2}));
    assert.writeOK(coll.insert({x: 1, y: 2}));
    assert.writeOK(coll.insert({x: 2, y: 1}));
    assert.writeOK(coll.insert({x: 2, y: 3}));

    // Chunks after merge:
    // (MinKey, { x: 0, y: 1 })
    // ({ x: 0, y: 1 }, { x: 2, y: 0 })
    // ({ x: 2, y: 0 }, { x: 2, y: 1 })
    // ({ x: 2, y: 1 }, MaxKey)
    jsTest.log("Merge chunks whose upper and lower bounds are compound shard keys.");
    checkMergeWorked({x: 0, y: 1}, {x: 2, y: 0});

    // Chunks after merge:
    // (MinKey, { x: 2, y: 0 })
    // ({ x: 2, y: 0 }, { x: 2, y: 1 })
    // ({ x: 2, y: 1 }, MaxKey)
    jsTest.log(
        "Merge chunks whose upper bound contains a compound shard key, lower bound is MinKey");
    checkMergeWorked({x: MinKey, y: MinKey}, {x: 2, y: 0});

    // Chunks after merge:
    // (MinKey, { x: 2, y: 0 })
    // ({ x: 2, y: 0 }, MaxKey)
    jsTest.log(
        "Merge chunks whose lower bound contains a compound shard key, upper bound is MaxKey");
    checkMergeWorked({x: 2, y: 0}, {x: MaxKey, y: MaxKey});

    // Chunks after merge:
    // (MinKey, MaxKey)
    jsTest.log("Merge chunks whos bounds are MinKey/MaxKey, but which have a compound shard key");
    checkMergeWorked({x: MinKey, y: MinKey}, {x: MaxKey, y: MaxKey});

    st.stop();

})();
