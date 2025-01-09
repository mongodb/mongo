/*
 * Tests that when an entry in config.cache.collections is corrupted,
 * the catalog cache refresh succeeds and after it finishes all persisted cache entries are intact.
 */

const st = new ShardingTest({shards: 1, rs: {nodes: 3}, initiateWithDefaultElectionTimeout: true});

assert.commandWorked(
    st.s.adminCommand({enableSharding: 'test', primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));

assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {_flushRoutingTableCacheUpdatesWithWriteConcern: 'test.foo', writeConcern: {w: 3}}));

const requiredFieldsNames = ["epoch", "timestamp", "uuid", "key", "unique"];

requiredFieldsNames.forEach(fieldName => {
    // Corrupt an entry in config.cache.collections by making it miss a required field.
    assert.commandWorked(
        st.rs0.getPrimary().getCollection('config.cache.collections').update({_id: 'test.foo'}, {
            $unset: {[fieldName]: true}
        }));

    // Assert that there exists a corrupted entry in config.cache.collections.
    assert.eq(1,
              st.rs0.getPrimary()
                  .getCollection('config.cache.collections')
                  .find({[fieldName]: {$exists: false}})
                  .itcount());

    // Ensure that the node acting as primary has a clean in-memory state in the SSCCL,
    // and there are no ongoing refreshes pending.
    assert.commandWorked(st.rs0.getPrimary().adminCommand({replSetStepDown: 5, force: true}));

    // Ensure the catalog cache refresh works despite config.cache.collections being corrupted.
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: 'test.foo'}));

    // Assert that after the refresh all entries in config.cache.collections are intact.
    assert.eq(0,
              st.rs0.getPrimary()
                  .getCollection('config.cache.collections')
                  .find({$or: requiredFieldsNames.map(f => ({[f]: {$exists: false}}))})
                  .itcount());
});

st.stop();
