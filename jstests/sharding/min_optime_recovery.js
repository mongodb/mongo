/**
 * Tests that the minOpTimeRecovery document will be created after a migration only
 * if the config server is a replica set.
 */
(function() {
"use strict";

var st = new ShardingTest({ shards: 2 });

var testDB = st.s.getDB('test');
testDB.adminCommand({ enableSharding: 'test' });
st.ensurePrimaryShard('test', 'shard0000');
testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});
testDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0001' });

var shardAdmin = st.d0.getDB('admin');
var doc = shardAdmin.system.version.findOne();

if (st.configRS) {
    assert.neq(null, doc);
    assert.eq('minOpTimeRecovery', doc._id);
    assert.eq(st.configRS.getURL(), doc.configsvrConnectionString);
    assert.eq('shard0000', doc.shardName);
    assert.gt(doc.minOpTime.ts.getTime(), 0);
}
else {
    assert.eq(null, doc);
}

st.stop();

})();
