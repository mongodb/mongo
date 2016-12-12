// Confirms proper behavior when reading from a view that is based on a sharded collection.
// TODO SERVER-24762: Add tests that confirm appropriate error when performing view query:
//       a) directly against non-primary shard
//       b) directly against primary shard primary
//       c) directly against primary shard secondary
// TODO SERVER-24762: Add explain shell helper tests.

(function() {
    "use strict";

    let st = new ShardingTest({name: "views_sharded", shards: 2, other: {enableBalancer: false}});

    let mongos = st.s;
    let config = mongos.getDB("config");
    let db = mongos.getDB(jsTestName());
    db.dropDatabase();

    let coll = db.getCollection("coll");

    assert.commandWorked(config.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), "shard0000");
    assert.commandWorked(config.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

    assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {a: 6}}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {a: 25}, to: "shard0001"}));

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    assert.commandWorked(db.createView("view", coll.getName(), [{$match: {a: {$gte: 4}}}]));
    let view = db.getCollection("view");

    //
    // find
    //
    assert.eq(5, view.find({a: {$lte: 8}}).itcount());

    let result = db.runCommand({explain: {find: "view", filter: {a: {$lte: 7}}}});
    assert.commandWorked(result);
    assert(result.hasOwnProperty("shards"), tojson(result));

    //
    // aggregate
    //
    assert.eq(5, view.aggregate([{$match: {a: {$lte: 8}}}]).itcount());

    result =
        db.runCommand({aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], explain: true});
    assert.commandWorked(result);
    assert(result.hasOwnProperty("shards"), tojson(result));

    //
    // count
    //
    assert.eq(5, view.count({a: {$lte: 8}}));

    result = db.runCommand({explain: {count: "view", query: {a: {$lte: 8}}}});
    assert.commandWorked(result);
    assert(result.hasOwnProperty("shards"), tojson(result));

    //
    // distinct
    //
    result = db.runCommand({distinct: "view", key: "a", query: {a: {$lte: 8}}});
    assert.commandWorked(result);
    assert.eq([4, 5, 6, 7, 8], result.values.sort());

    result = db.runCommand({explain: {distinct: "view", key: "a", query: {a: {$lte: 8}}}});
    assert.commandWorked(result);
    assert(result.hasOwnProperty("shards"), tojson(result));

    //
    // Confirm cleanupOrphaned command fails.
    //
    result = st.getPrimaryShard(db.getName()).getDB("admin").runCommand({
        cleanupOrphaned: view.getFullName()
    });
    assert.commandFailedWithCode(result, ErrorCodes.CommandNotSupportedOnView);

    //
    //  Confirm getShardVersion command fails.
    //
    assert.commandFailedWithCode(db.adminCommand({getShardVersion: view.getFullName()}),
                                 ErrorCodes.NamespaceNotSharded);

})();
