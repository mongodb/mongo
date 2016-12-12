/**
 * Tests the correctness of sharding initialization through setShardVersion.
 *
 * Though sharding initialization is typically done:
 *
 * 1) when the config server inserts the shardIdentity document on a new shard, or
 * 2) when the shard starts up with a shardIdentity document already on disk
 *
 * the initialization may be done through setShardVersion if a sharded connection from a mongos or
 * config is made to the new shard before the shardIdentity insert triggers sharding initialization.
 */
(function() {
    'use strict';

    // Prevent a config primary from upserting the shardIdentity document into the shards by using
    // the dontUpsertShardIdentityOnNewShards failpoint.
    var st = new ShardingTest({
        shards: 1,
        mongos: 1,
        other: {
            rs: true,
            rsOptions: {nodes: 1},
            configOptions: {
                setParameter:
                    {"failpoint.dontUpsertShardIdentityOnNewShards": "{'mode':'alwaysOn'}"}
            }
        }
    });

    st.configRS.awaitReplication();
    var configVersion = st.s.getDB('config').getCollection('version').findOne();
    assert.neq(null, configVersion);
    var clusterId = configVersion.clusterId;
    assert.neq(null, clusterId);

    // The balancer, even when disabled, initiates a sharded connection to each new shard through
    // its periodic check that no shards' process OIDs clash. Expect that this check will send
    // setShardVersion and trigger sharding initialization on the new shard soon.
    var fiveMinutes = 30000;
    assert.soon(function() {
        var res = st.rs0.getPrimary().adminCommand({shardingState: 1});
        assert.commandWorked(res);
        if (res.enabled) {
            // If sharding state was initialized, make sure all fields are correct. Note, the
            // clusterId field is not initialized through setShardVersion.
            return (st.configRS.getURL() === res.configServer) && (st.rs0.name === res.shardName) &&
                (!clusterId.equals(res.clusterId));
        } else {
            return false;
        }
    }, "Shard failed to initialize sharding awareness after being added as a shard", fiveMinutes);

    // Assert that the shardIdentity document was not somehow inserted on the shard, triggering
    // sharding initialization unexpectedly.
    var res = st.rs0.getPrimary().getDB("admin").getCollection("system.version").findOne({
        _id: "shardIdentity"
    });
    assert.eq(null, res);

    st.stop();

})();
