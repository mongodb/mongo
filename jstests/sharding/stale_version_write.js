// Tests whether a reset sharding version triggers errors
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, bongos: 2});

    var bongosA = st.s0;
    var bongosB = st.s1;

    jsTest.log("Adding new collections...");

    var collA = bongosA.getCollection(jsTestName() + ".coll");
    assert.writeOK(collA.insert({hello: "world"}));

    var collB = bongosB.getCollection("" + collA);
    assert.writeOK(collB.insert({hello: "world"}));

    jsTest.log("Enabling sharding...");

    assert.commandWorked(bongosA.getDB("admin").adminCommand({enableSharding: "" + collA.getDB()}));
    assert.commandWorked(
        bongosA.getDB("admin").adminCommand({shardCollection: "" + collA, key: {_id: 1}}));

    // BongoD doesn't know about the config shard version *until* BongoS tells it
    collA.findOne();

    jsTest.log("Trigger shard version mismatch...");

    assert.writeOK(collB.insert({goodbye: "world"}));

    print("Inserted...");

    assert.eq(3, collA.find().itcount());
    assert.eq(3, collB.find().itcount());

    st.stop();
})();
