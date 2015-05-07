/*
 * Test that migrations still succeed after a shard primary steps down and is
 * re-elected again.
 *
 * This test is composed of 3 sub-tests:
 * - When the "from" shard has stepped down and been re-elected.
 * - (*) When the "to" shard has stepped down and been re-elected.
 * - (*) When both the "from" and "to" shards have stepped down and been re-elected.
 *
 * (*) Not in v2.6, due to SERVER-15022.
 *
 * Each sub-test does:
 * - Setup basic sharded collection, 2 shards, 2 chunks.
 * - Force a brief stepdown and re-election of the "from" and/or "to" shard primary.
 * - Migrate a chunk.
 */

(function() {
"use strict";

var testShardPrimaryRelinquishMigrate = function(opts) {
    jsTestLog("START testShardPrimaryRelinquishMigrate(" + tojson(opts) + ")");

    // The shards need to have specific priorities in the replset configs.
    // Hence, create the ShardingTest with no shards, and then manually add
    // each shard (with the correct replset config).
    var st = new ShardingTest({ shards: [], mongos: 1, config: 3,
                                other: { smallfiles: true } });

    var mongos = st.s0;
    var admin = mongos.getDB( "admin" );

    var doAddShard = function (shardNum) {
        var rs = new ReplSetTest({ name: "shard" + shardNum,
                                   nodes: 2,
                                   startPort: 31100 + ( shardNum * 100 ),
                                   useHostName: false,
                                   shardSvr: true });
        rs.startSet();
        var cfg = rs.getReplSetConfig();
        cfg.members[1].priority = 0;
        rs.initiate(cfg);
        assert.commandWorked( admin.runCommand( { addShard: rs.getURL() } ) );
        st["rs" + shardNum] = rs;
    };
    doAddShard(0);
    doAddShard(1);

    var shards = mongos.getCollection( "config.shards" ).find().toArray();
    var databases = mongos.getCollection( "config.databases" );
    var coll = mongos.getCollection( "foo.bar" );
    var collName = coll.getFullName();
    var dbName = coll.getDB().getName();

    st.stopBalancer();

    assert.commandWorked( admin.runCommand({ enableSharding: dbName }) );
    var dbRecord = databases.find({_id: dbName}).next();
    assert(dbRecord && "primary" in dbRecord);
    if (dbRecord.primary != shards[0]._id) {
        assert.commandWorked( admin.runCommand({ movePrimary: dbName,
                                                 to: shards[0]._id }) );
    }
    assert.commandWorked( admin.runCommand({ shardCollection: collName,
                                             key: { _id: 1 } }) );
    assert.commandWorked( admin.runCommand({ split: collName,
                                             middle: { _id: 0 } }) );

    coll.insert({_id: -1});
    coll.insert({_id: 1});
    assert.eq( coll.count(), 2 );

    // Move chunk there and back to initialise sharding (and this sharded
    // collection) on both shards.
    assert.commandWorked( admin.runCommand({ moveChunk: collName,
                                             find: { _id: 0 },
                                             to: shards[1]._id,
                                             _waitForDelete: true }) );

    assert.commandWorked( admin.runCommand({ moveChunk: collName,
                                             find: { _id: 0 },
                                             to: shards[0]._id,
                                             _waitForDelete: true }) );

    // Force the primary to step down briefly, and wait for it to come back
    // (since the other member is priority:0).
    var bounce = function(name, rs) {
        jsTestLog("START bouncing " + name + " shard");
        var primary = rs.getPrimary();
        assert(primary, "rs.getPrimary() failed");
        var res;
        try {
            // replSetStepDown should cause an exception
            // (when the primary drops the connection).
            res = primary.adminCommand({ replSetStepDown: 1,
                                         secondaryCatchUpPeriodSecs: 0,
                                         force: true });
        } catch(e) {
            print("Expected exception for replSetStepdown: " + e);
        }
        // Check if replSetStepDown has returned anything
        // (rather than throwing the expected exception).
        if (typeof(res) != "undefined") {
            throw("Unexpected return from replSetStepDown: " + tojson(res));
        }
        rs.waitForMaster();
        jsTestLog("END bouncing " + name + " shard");
    };

    if (opts.bounceFrom) {
        bounce("FROM", st.rs0);
    }

    if (opts.bounceTo) {
        bounce("TO", st.rs1);
    }

    jsTestLog("START migration");
    assert.commandWorked( admin.runCommand({ moveChunk: collName,
                                             find: { _id: 0 },
                                             to: shards[1]._id,
                                             _waitForDelete: true }) );
    jsTestLog("END migration");

    st.stop();

    jsTestLog("END testShardPrimaryRelinquishMigrate(" + tojson(opts) + ")");
};

testShardPrimaryRelinquishMigrate({ bounceFrom: true, bounceTo: false });

// These two sub-tests are disabled in v2.6, because bouncing the To shard
// fails due to SERVER-15022 (which hasn't been backported).
//testShardPrimaryRelinquishMigrate({ bounceFrom: false, bounceTo: true });
//testShardPrimaryRelinquishMigrate({ bounceFrom: true, bounceTo: true });

}());
