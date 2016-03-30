//
// Tests cleanup of orphaned data in hashed sharded coll via the orphaned data cleanup command
//

(function() {
    "use strict";

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var coll = mongos.getCollection("foo.bar");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: st.shard0.shardName}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: "hashed"}}));

    // Create two orphaned data holes, one bounded by min or max on each shard

    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: NumberLong(-100)}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: NumberLong(-50)}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: NumberLong(50)}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: NumberLong(100)}}));
    assert.commandWorked(admin.runCommand({
        moveChunk: coll + "",
        bounds: [{_id: NumberLong(-100)}, {_id: NumberLong(-50)}],
        to: st.shard1.shardName,
        _waitForDelete: true
    }));
    assert.commandWorked(admin.runCommand({
        moveChunk: coll + "",
        bounds: [{_id: NumberLong(50)}, {_id: NumberLong(100)}],
        to: st.shard0.shardName,
        _waitForDelete: true
    }));
    st.printShardingStatus();

    jsTest.log("Inserting some docs on each shard, so 1/2 will be orphaned...");

    for (var s = 0; s < 2; s++) {
        var shardColl = (s == 0 ? st.shard0 : st.shard1).getCollection(coll + "");
        var bulk = shardColl.initializeUnorderedBulkOp();
        for (var i = 0; i < 100; i++)
            bulk.insert({_id: i});
        assert.writeOK(bulk.execute());
    }

    assert.eq(200,
              st.shard0.getCollection(coll + "").find().itcount() +
                  st.shard1.getCollection(coll + "").find().itcount());
    assert.eq(100, coll.find().itcount());

    jsTest.log("Cleaning up orphaned data in hashed coll...");

    for (var s = 0; s < 2; s++) {
        var shardAdmin = (s == 0 ? st.shard0 : st.shard1).getDB("admin");

        var result = shardAdmin.runCommand({cleanupOrphaned: coll + ""});
        while (result.ok && result.stoppedAtKey) {
            printjson(result);
            result = shardAdmin.runCommand(
                {cleanupOrphaned: coll + "", startingFromKey: result.stoppedAtKey});
        }

        printjson(result);
        assert(result.ok);
    }

    assert.eq(100,
              st.shard0.getCollection(coll + "").find().itcount() +
                  st.shard1.getCollection(coll + "").find().itcount());
    assert.eq(100, coll.find().itcount());

    jsTest.log("DONE!");

    st.stop();

})();
