// test that the mongos doesn't segfault when it receives malformed BSON
var st = new ShardingTest({shards:1});
var testDB = st.getDB('test');
testDB.test.insert({a:1});

try {
    testDB.test.find({key: {$regex: 'abcd\0xyz'}}).explain();
} catch (e) {
    /*
     * if the mongos segfaults, the error is the msg:
     * "Error: error doing query: failed: network error while attempting to run command 'explain' on host '127.0.0.1:20014'"
     *
     * if the mongos doesn't segfault, the error is the object:
     * "Error: explain failed: {
     *   "code" : 22,
     *   "ok" : 0,
     *   "errmsg" : "bson length doesn't match what we found in object with unknown _id"
     * }"
     */
    assert.eq(22, e.code);
}