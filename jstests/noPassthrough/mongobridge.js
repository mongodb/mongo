/**
 * Test that mongobridge's *From commands work
 *
 * @tags: [requires_replication,requires_sharding,live_record_incompatible]
 */

// mongobridge depends on test commands being enabled. Also EVERY repl/sharding
// test depends on this. Think twice if you're thinking of changing the default.
assert.eq(jsTest.options().enableTestCommands, true);

// we expect this to work just fine given that enableTestCommands is true by default
var st = new ShardingTest({
    shards: {rs0: {nodes: 2}},
    mongos: 1,
    config: 1,
    useBridge: true,
    rsOptions: {settings: {electionTimeoutMillis: 60000}}
});
var wc = {writeConcern: {w: 2, wtimeout: 4000}};

// delayMessagesFrom should cause a write error on this insert
st.rs0.getPrimary().delayMessagesFrom(st.rs0.getSecondary(), 13000);
assert.commandFailed(st.s0.getCollection('testDB.cll').insert({test: 5}, wc));
st.rs0.getPrimary().delayMessagesFrom(st.rs0.getSecondary(), 0);

// discardMessages w/ a loss probabilty of 1 should also cause a write error
st.rs0.getPrimary().discardMessagesFrom(st.rs0.getSecondary(), 1.0);
assert.commandFailed(st.s0.getCollection('testDB.cll').insert({test: 5}, wc));
st.rs0.getPrimary().discardMessagesFrom(st.rs0.getSecondary(), 0.0);

// reject connections should make the write fail as well
st.rs0.getPrimary().rejectConnectionsFrom(st.rs0.getSecondary());
assert.commandFailed(st.s0.getCollection('testDB.cll').insert({test: 5}, wc));
// but if we make mongobridge accept the connections, the write should now
// succeed
st.rs0.getPrimary().acceptConnectionsFrom(st.rs0.getSecondary());
// don't use wtimeout with this command. Slower boxes can be busy catching up
assert.commandWorked(st.s0.getCollection('testDB.cll').insert({test: 5}, {writeConcern: {w: 2}}));

// None of the above commands prevent writes to the primary, so we still
// expect to find 4 documents
assert.eq(st.s0.getCollection('testDB.cll').find({test: 5}).length(), 4);
st.stop();
