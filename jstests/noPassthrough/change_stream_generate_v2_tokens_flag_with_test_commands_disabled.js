/**
 * Test that the $_generateV2ResumeTokens parameter cannot be used on mongoS when test commands are
 * disabled.
 * @tags: [
 *   uses_change_streams,
 *   requires_sharding,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

// Signal to the ShardingTest that we want to disable test commands.
TestData.enableTestCommands = false;

// Create a sharding fixture with test commands disabled.
const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

// Confirm that attempting to set any values for $_generateV2ResumeTokens field fails on mongos.
assert.throwsWithCode(() => st.s.watch([], {$_generateV2ResumeTokens: true}).hasNext(), 6528201);
assert.throwsWithCode(() => st.s.watch([], {$_generateV2ResumeTokens: false}).hasNext(), 6528201);

// Confirm that attempting to run change streams with $_generateV2ResumeTokens:true fails on shards.
assert.throwsWithCode(
    () => st.rs0.getPrimary().watch([], {$_generateV2ResumeTokens: true}).hasNext(), 6528200);

// Explicity requesting v1 tokens is allowed on a shard. This is to allow a 6.0 mongoS to
// communicate with a 7.0 shard.
const stream = st.rs0.getPrimary().watch([], {$_generateV2ResumeTokens: false});
assert.commandWorked(st.s.getDB("test")["coll"].insert({x: 1}));
assert.soon(() => stream.hasNext());

st.stop();
})();
