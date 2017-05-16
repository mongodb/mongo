// Ensure you can't shard on an array key
(function() {
    'use strict';

    var st = new ShardingTest({shards: 3});

    var mongos = st.s0;

    var coll = mongos.getCollection("TestDB.foo");

    st.shardColl(coll, {_id: 1, i: 1}, {_id: ObjectId(), i: 1});

    printjson(mongos.getDB("config").chunks.find().toArray());

    print("1: insert some invalid data");

    var value = null;

    // Insert an object with invalid array key
    assert.writeError(coll.insert({i: [1, 2]}));

    // Insert an object with all the right fields, but an invalid array val for _id
    assert.writeError(coll.insert({_id: [1, 2], i: 3}));

    // Insert an object with valid array key
    assert.writeOK(coll.insert({i: 1}));

    // Update the value with valid other field
    value = coll.findOne({i: 1});
    assert.writeOK(coll.update(value, {$set: {j: 2}}));

    // Update the value with invalid other fields
    value = coll.findOne({i: 1});
    assert.writeError(coll.update(value, Object.merge(value, {i: [3]})));

    // Multi-update the value with invalid other fields
    value = coll.findOne({i: 1});
    assert.writeError(coll.update(value, Object.merge(value, {i: [3, 4]}), false, true));

    // Multi-update the value with other fields (won't work, but no error)
    value = coll.findOne({i: 1});
    assert.writeOK(coll.update(Object.merge(value, {i: [1, 1]}), {$set: {k: 4}}, false, true));

    // Query the value with other fields (won't work, but no error)
    value = coll.findOne({i: 1});
    coll.find(Object.merge(value, {i: [1, 1]})).toArray();

    // Can't remove using multikey, but shouldn't error
    value = coll.findOne({i: 1});
    coll.remove(Object.extend(value, {i: [1, 2, 3, 4]}));

    // Can't remove using multikey, but shouldn't error
    value = coll.findOne({i: 1});
    assert.writeOK(coll.remove(Object.extend(value, {i: [1, 2, 3, 4, 5]})));
    assert.eq(coll.find().itcount(), 1);

    value = coll.findOne({i: 1});
    assert.writeOK(coll.remove(Object.extend(value, {i: 1})));
    assert.eq(coll.find().itcount(), 0);

    coll.ensureIndex({_id: 1, i: 1, j: 1});
    // Can insert document that will make index into a multi-key as long as it's not part of shard
    // key.
    coll.remove({});
    assert.writeOK(coll.insert({i: 1, j: [1, 2]}));
    assert.eq(coll.find().itcount(), 1);

    // Same is true for updates.
    coll.remove({});
    coll.insert({_id: 1, i: 1});
    assert.writeOK(coll.update({_id: 1, i: 1}, {_id: 1, i: 1, j: [1, 2]}));
    assert.eq(coll.find().itcount(), 1);

    // Same for upserts.
    coll.remove({});
    assert.writeOK(coll.update({_id: 1, i: 1}, {_id: 1, i: 1, j: [1, 2]}, true));
    assert.eq(coll.find().itcount(), 1);

    printjson(
        "Sharding-then-inserting-multikey tested, now trying inserting-then-sharding-multikey");

    // Insert a bunch of data then shard over key which is an array
    var coll = mongos.getCollection("" + coll + "2");
    for (var i = 0; i < 10; i++) {
        // TODO : does not check weird cases like [ i, i ]
        assert.writeOK(coll.insert({i: [i, i + 1]}));
    }

    coll.ensureIndex({_id: 1, i: 1});

    try {
        st.shardColl(coll, {_id: 1, i: 1}, {_id: ObjectId(), i: 1});
    } catch (e) {
        print("Correctly threw error on sharding with multikey index.");
    }

    st.printShardingStatus();

    // Insert a bunch of data then shard over key which is not an array
    var coll = mongos.getCollection("" + coll + "3");
    for (var i = 0; i < 10; i++) {
        // TODO : does not check weird cases like [ i, i ]
        assert.writeOK(coll.insert({i: i}));
    }

    coll.ensureIndex({_id: 1, i: 1});

    st.shardColl(coll, {_id: 1, i: 1}, {_id: ObjectId(), i: 1});

    st.printShardingStatus();

    st.stop();
})();
