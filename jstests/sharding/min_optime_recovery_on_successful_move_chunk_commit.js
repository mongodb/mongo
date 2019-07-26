/**
 * Tests that the minOpTimeRecovery document will be created after a migration.
 */
(function() {
"use strict";

var st = new ShardingTest({shards: 2});

var testDB = st.s.getDB('test');
testDB.adminCommand({enableSharding: 'test'});
st.ensurePrimaryShard('test', st.shard0.shardName);
testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});

var priConn = st.configRS.getPrimary();
var replStatus = priConn.getDB('admin').runCommand({replSetGetStatus: 1});
replStatus.members.forEach(function(memberState) {
    if (memberState.state == 1) {  // if primary
        assert.neq(null, memberState.optime);
        assert.neq(null, memberState.optime.ts);
        assert.neq(null, memberState.optime.t);
    }
});

testDB.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.shardName});

var shardAdmin = st.rs0.getPrimary().getDB('admin');
var minOpTimeRecoveryDoc = shardAdmin.system.version.findOne({_id: 'minOpTimeRecovery'});

assert.neq(null, minOpTimeRecoveryDoc);
assert.eq('minOpTimeRecovery', minOpTimeRecoveryDoc._id);
assert.eq(st.configRS.getURL(),
          minOpTimeRecoveryDoc.configsvrConnectionString);       // TODO SERVER-34166: Remove.
assert.eq(st.shard0.shardName, minOpTimeRecoveryDoc.shardName);  // TODO SERVER-34166: Remove.
assert.gt(minOpTimeRecoveryDoc.minOpTime.ts.getTime(), 0);
assert.eq(0, minOpTimeRecoveryDoc.minOpTimeUpdaters);

st.stop();
})();
