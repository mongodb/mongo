/**
 * Test that mongobridge's *From commands succeed when test commands are
 * enabled, and fail when disabled.
 *
 * @tags: [requires_replication,requires_sharding,live_record_incompatible]
 */

// mongobridge depends on test commands being enabled. Also EVERY repl/sharding
// test depends on this. Think twice if you're thinking of changing the default.
assert.eq(jsTest.options().enableTestCommands, true);

// we expect this to work just fine given that enableTestCommands is true by default
var st = new ShardingTest({shards: {rs0: {nodes: 2}}, mongos: 1, config: 1, useBridge: true});

// changing enableTestcommands should have no impact on the existing mongod/s/bridge instances,
TestData.enableTestCommands = false;
st.rs0.getSecondary().delayMessagesFrom(st.rs0.getPrimary(), 13000);
st.rs0.getSecondary().discardMessagesFrom(st.rs0.getPrimary(), 1.0);
st.rs0.getSecondary().acceptConnectionsFrom(st.rs0.getPrimary());
st.rs0.getSecondary().rejectConnectionsFrom(st.rs0.getPrimary());
st.stop();

// Start another test, this time with enableTestCommands as false.
// Repeating the above, we expect the commands to fail
st = new ShardingTest({shards: {rs0: {nodes: 2}}, mongos: 1, config: 1, useBridge: true});
assert.throws(() => {
    st.rs0.getSecondary().delayMessagesFrom(st.rs0.getPrimary(), 13000);
});
assert.throws(() => {
    st.rs0.getSecondary().discardMessagesFrom(st.rs0.getPrimary(), 1.0);
});
assert.throws(() => {
    st.rs0.getSecondary().acceptConnectionsFrom(st.rs0.getPrimary());
});
assert.throws(() => {
    st.rs0.getSecondary().rejectConnectionsFrom(st.rs0.getPrimary());
});
st.stop();
