/**
 * Test that the $_generateV2ResumeTokens parameter cannot be used when test commands are disabled.
 * @tags: [
 *   uses_change_streams,
 *   requires_sharding,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

// Signal to the ShardingTest that we want to disable test commands.
TestData.enableTestCommands = false;

// Create a sharding fixture with test commands disabled.
const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

// Confirm that attempting to run change streams with $_generateV2ResumeTokens:true fails on mongos.
assert.throwsWithCode(() => st.s.watch([], {$_generateV2ResumeTokens: true}).hasNext(), 6528201);

// Confirm that attempting to run change streams with $_generateV2ResumeTokens:true fails on shards.
assert.throwsWithCode(
    () => st.rs0.getPrimary().watch([], {$_generateV2ResumeTokens: true}).hasNext(), 6528200);

st.stop();
})();