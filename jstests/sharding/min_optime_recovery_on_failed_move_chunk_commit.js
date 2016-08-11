/**
 * Tests that the shard will update the min optime recovery document after startup.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 1});

    // Insert a recovery doc with non-zero minOpTimeUpdaters to simulate a migration
    // process that crashed in the middle of the critical section.

    var recoveryDoc = {
        _id: 'minOpTimeRecovery',
        configsvrConnectionString: st.configRS.getURL(),
        shardName: 'shard0000',
        minOpTime: {ts: Timestamp(0, 0), t: 0},
        minOpTimeUpdaters: 2
    };

    assert.writeOK(st.d0.getDB('admin').system.version.insert(recoveryDoc));

    // Make sure test is setup correctly.
    var minOpTimeRecoveryDoc =
        st.d0.getDB('admin').system.version.findOne({_id: 'minOpTimeRecovery'});

    assert.neq(null, minOpTimeRecoveryDoc);
    assert.eq(0, minOpTimeRecoveryDoc.minOpTime.ts.getTime());
    assert.eq(2, minOpTimeRecoveryDoc.minOpTimeUpdaters);

    st.restartMongod(0);

    // After the restart, the shard should have updated the opTime and reset minOpTimeUpdaters.
    minOpTimeRecoveryDoc = st.d0.getDB('admin').system.version.findOne({_id: 'minOpTimeRecovery'});

    assert.neq(null, minOpTimeRecoveryDoc);
    assert.gt(minOpTimeRecoveryDoc.minOpTime.ts.getTime(), 0);
    assert.eq(0, minOpTimeRecoveryDoc.minOpTimeUpdaters);

    st.stop();

})();
