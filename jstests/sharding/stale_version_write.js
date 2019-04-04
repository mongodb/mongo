// Tests whether a reset sharding version triggers errors
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, merizos: 2});

    var merizosA = st.s0;
    var merizosB = st.s1;

    jsTest.log("Adding new collections...");

    var collA = merizosA.getCollection(jsTestName() + ".coll");
    assert.writeOK(collA.insert({hello: "world"}));

    var collB = merizosB.getCollection("" + collA);
    assert.writeOK(collB.insert({hello: "world"}));

    jsTest.log("Enabling sharding...");

    assert.commandWorked(merizosA.getDB("admin").adminCommand({enableSharding: "" + collA.getDB()}));
    assert.commandWorked(
        merizosA.getDB("admin").adminCommand({shardCollection: "" + collA, key: {_id: 1}}));

    // MongoD doesn't know about the config shard version *until* MongoS tells it
    collA.findOne();

    jsTest.log("Trigger shard version mismatch...");

    assert.writeOK(collB.insert({goodbye: "world"}));

    print("Inserted...");

    assert.eq(3, collA.find().itcount());
    assert.eq(3, collB.find().itcount());

    st.stop();
})();
