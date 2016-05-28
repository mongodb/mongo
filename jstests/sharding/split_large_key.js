// Test for splitting a chunk with a very large shard key value should not be allowed
// and does not corrupt the config.chunks metadata.
(function() {
    'use strict';

    function verifyChunk(keys, expectFail) {
        // If split failed then there's only 1 chunk
        // With a min & max for the shardKey
        if (expectFail) {
            assert.eq(1, configDB.chunks.find().count(), "Chunks count no split");
            var chunkDoc = configDB.chunks.findOne();
            assert.eq(0, bsonWoCompare(chunkDoc.min, keys.min), "Chunks min");
            assert.eq(0, bsonWoCompare(chunkDoc.max, keys.max), "Chunks max");
        } else {
            assert.eq(2, configDB.chunks.find().count(), "Chunks count split");
        }
    }

    // Tests
    //  - name: Name of test, used in collection name
    //  - key: key to test
    //  - keyFieldSize: size of each key field
    //  - expectFail: true/false, true if key is too large to pre-split
    var tests = [
        {name: "Key size small", key: {x: 1}, keyFieldSize: 100, expectFail: false},
        {name: "Key size 512", key: {x: 1}, keyFieldSize: 512, expectFail: true},
        {name: "Key size 2000", key: {x: 1}, keyFieldSize: 2000, expectFail: true},
        {name: "Compound key size small", key: {x: 1, y: 1}, keyFieldSize: 100, expectFail: false},
        {name: "Compound key size 512", key: {x: 1, y: 1}, keyFieldSize: 256, expectFail: true},
        {name: "Compound key size 10000", key: {x: 1, y: 1}, keyFieldSize: 5000, expectFail: true},
    ];

    var st = new ShardingTest({shards: 1});
    var configDB = st.s.getDB('config');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));

    tests.forEach(function(test) {
        var collName = "split_large_key_" + test.name;
        var midKey = {};
        var chunkKeys = {min: {}, max: {}};
        for (var k in test.key) {
            // new Array with join creates string length 1 less than size, so add 1
            midKey[k] = new Array(test.keyFieldSize + 1).join('a');
            // min & max keys for each field in the index
            chunkKeys.min[k] = MinKey;
            chunkKeys.max[k] = MaxKey;
        }

        assert.commandWorked(
            configDB.adminCommand({shardCollection: "test." + collName, key: test.key}));

        var res = configDB.adminCommand({split: "test." + collName, middle: midKey});
        if (test.expectFail) {
            assert(!res.ok, "Split: " + collName);
            assert(res.errmsg !== null, "Split errmsg: " + collName);
        } else {
            assert(res.ok, "Split: " + collName + " " + res.errmsg);
        }

        verifyChunk(chunkKeys, test.expectFail);

        st.s0.getCollection("test." + collName).drop();
    });

    st.stop();

})();
