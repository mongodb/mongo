/**
 * Tests that the minOpTimeRecovery document will be created after a migration.
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 2});

    var testDB = st.s.getDB('test');
    testDB.adminCommand({enableSharding: 'test'});
    st.ensurePrimaryShard('test', 'shard0000');
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

    testDB.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: 'shard0001'});

    var shardAdmin = st.d0.getDB('admin');
    var minOpTimeRecoveryDoc = shardAdmin.system.version.findOne({_id: 'minOpTimeRecovery'});

    assert.neq(null, minOpTimeRecoveryDoc);
    assert.eq('minOpTimeRecovery', minOpTimeRecoveryDoc._id);
    assert.eq(st.configRS.getURL(), minOpTimeRecoveryDoc.configsvrConnectionString);
    assert.eq('shard0000', minOpTimeRecoveryDoc.shardName);
    assert.gt(minOpTimeRecoveryDoc.minOpTime.ts.getTime(), 0);

    st.stop();

})();
