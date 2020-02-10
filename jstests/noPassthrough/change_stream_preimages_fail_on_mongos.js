/**
 * Test that mongoS rejects change streams which request 'fullDocumentBeforeChange' pre-images.
 *
 * @tags: [uses_change_streams, requires_replication]
 */
(function() {
'use strict';

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    config: 1,
});

const shard = st.shard0;
const mongos = st.s;

// Test that we cannot create a collection with pre-images enabled in a sharded cluster.
assert.commandFailed(shard.getDB("test").runCommand({create: "test", recordPreImages: true}));

// Test that attempting to run $changeStream with {fullDocumentBeforeChange: "whenAvailable"} fails.
assert.commandFailedWithCode(mongos.getDB("test").runCommand({
    aggregate: 1,
    pipeline: [{$changeStream: {fullDocumentBeforeChange: "whenAvailable"}}],
    cursor: {}
}),
                             51771);

// Test that attempting to run $changeStream with {fullDocumentBeforeChange: "required"} fails.
assert.commandFailedWithCode(mongos.getDB("test").runCommand({
    aggregate: 1,
    pipeline: [{$changeStream: {fullDocumentBeforeChange: "required"}}],
    cursor: {}
}),
                             51771);

st.stop();
}());
