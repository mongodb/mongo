// Test for splitting a chunk with a very large shard key value.
(function() {
'use strict';

// Tests
//  - name: Name of test, used in collection name
//  - key: key to test
//  - keyFieldSize: size of each key field
var tests = [
    {name: "Key size small", key: {x: 1}, keyFieldSize: 100},
    {name: "Key size 512", key: {x: 1}, keyFieldSize: 512},
    {name: "Key size 2000", key: {x: 1}, keyFieldSize: 2000},
    {name: "Compound key size small", key: {x: 1, y: 1}, keyFieldSize: 100},
    {name: "Compound key size 512", key: {x: 1, y: 1}, keyFieldSize: 256},
    {name: "Compound key size 10000", key: {x: 1, y: 1}, keyFieldSize: 5000},
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
    assert(res.ok, "Split: " + collName + " " + res.errmsg);

    assert.eq(2, configDB.chunks.find({"ns": "test." + collName}).count(), "Chunks count split");

    st.s0.getCollection("test." + collName).drop();
});

st.stop();
})();
