/**
 * Test that mongoS explicitly sets the value of $_generateV2ResumeTokens to 'false' on the commands
 * it sends to the shards if no value was specified by the client, and that if a value was
 * specified, it forwards it to the shards. On a replica set, no explicit value is set; the
 * aggregation simply treats it as default-false.
 * TODO SERVER-65370: remove or rework this test when v2 tokens become the default.
 * @tags: [
 *   uses_change_streams,
 *   requires_sharding,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
load("jstests/libs/profiler.js");                  // For profilerHasSingleMatchingEntryOrThrow.

// Create a sharding fixture with a single one-node replset shard and a one-node replset config
// server. The latter is to ensure that there is only one node that the internal new-shard monitor
// $changeStream can be sent to, since it is dispatched with secondaryPreferred readPreference.
const st = new ShardingTest({shards: 1, rs: {nodes: 1}, config: {nodes: 1}});

const mongosDB = st.s.getDB("test");
const shardDB = st.rs0.getPrimary().getDB(mongosDB.getName());
const configDB = st.configRS.getPrimary().getDB("config");

const mongosColl = assertDropAndRecreateCollection(mongosDB, jsTestName());
const shardColl = shardDB[mongosColl.getName()];
const configColl = configDB.shards;

// Enable profiling on the shard and config server.
assert.commandWorked(shardDB.setProfilingLevel(2));
assert.commandWorked(configDB.setProfilingLevel(2));

// Create one stream on mongoS that returns v1 tokens, the default.
const v1MongosStream = mongosColl.watch([], {comment: "v1MongosStream"});

// Create a second stream on mongoS that explicitly requests v2 tokens.
const v2MongosStream =
    mongosColl.watch([], {comment: "v2MongosStream", $_generateV2ResumeTokens: true});

// Create a stream directly on the shard which returns the default v1 tokens.
const v1ShardStream = shardColl.watch([], {comment: "v1ShardStream"});

// Insert a test document into the collection.
assert.commandWorked(mongosColl.insert({_id: 1}));

// Wait until all streams have encountered the insert operation.
assert.soon(() => v1MongosStream.hasNext() && v2MongosStream.hasNext() && v1ShardStream.hasNext());

// Confirm that in a sharded cluster, mongoS explicitly sets $_generateV2ResumeTokens to false on
// the command that it sends to the shards if nothing was specified by the client.
profilerHasAtLeastOneMatchingEntryOrThrow({
    profileDB: shardDB,
    filter: {
        "originatingCommand.aggregate": mongosColl.getName(),
        "originatingCommand.comment": "v1MongosStream",
        "originatingCommand.$_generateV2ResumeTokens": false
    }
});

// Confirm that we also set $_generateV2ResumeTokens to false on the internal new-shard monitoring
// $changeStream that we dispatch to the config servers.
profilerHasAtLeastOneMatchingEntryOrThrow({
    profileDB: configDB,
    filter: {
        "originatingCommand.aggregate": configColl.getName(),
        "originatingCommand.comment": "v1MongosStream",
        "originatingCommand.$_generateV2ResumeTokens": false
    }
});

// Confirm that mongoS correctly forwards the value of $_generateV2ResumeTokens to the shards if it
// is specified by the client.
profilerHasAtLeastOneMatchingEntryOrThrow({
    profileDB: shardDB,
    filter: {
        "originatingCommand.aggregate": mongosColl.getName(),
        "originatingCommand.comment": "v2MongosStream",
        "originatingCommand.$_generateV2ResumeTokens": true
    }
});

// Confirm that we also forward the value of $_generateV2ResumeTokens to the config servers.
profilerHasAtLeastOneMatchingEntryOrThrow({
    profileDB: configDB,
    filter: {
        "originatingCommand.aggregate": configColl.getName(),
        "originatingCommand.comment": "v2MongosStream",
        "originatingCommand.$_generateV2ResumeTokens": true
    }
});

// Confirm that on a replica set - in this case, a direct connection to the shard - no value is set
// for $_generateV2ResumeTokens if the client did not specify one. The aggregation defaults to
// treating the value as false.
profilerHasAtLeastOneMatchingEntryOrThrow({
    profileDB: shardDB,
    filter: {
        "originatingCommand.aggregate": mongosColl.getName(),
        "originatingCommand.comment": "v1ShardStream",
        "originatingCommand.$_generateV2ResumeTokens": {$exists: false}
    }
});

st.stop();
})();