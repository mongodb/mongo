/**
 * Tests the idempotency of the _configsvrCommitRefineCollectionShardKey command.
 *
 * @tags: [
 *   featureFlagAuthoritativeRefineCollectionShardKey,
 *   does_not_support_stepdowns,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

(function() {
'use strict';

function runConfigsvrCommitRefineCollectionShardKey(st, ns, oldTimestamp, newTimestamp, newKey) {
    return st.configRS.getPrimary().adminCommand({
        _configsvrCommitRefineCollectionShardKey: ns,
        key: newKey,
        newTimestamp: newTimestamp,
        newEpoch: new ObjectId(),
        oldTimestamp: oldTimestamp,
        writeConcern: {w: 'majority'}
    });
}

const st = new ShardingTest({shards: 1});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const initialShardKey = {
    x: 1
};
const newShardKey = {
    x: 1,
    y: 1
};

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: initialShardKey}));
const initialCollectionMetadata = st.s.getCollection('config.collections').findOne({_id: ns});

assert.eq(initialShardKey, initialCollectionMetadata.key);
assert.commandWorked(st.s.getDB(dbName).runCommand(
    {createIndexes: collName, indexes: [{key: newShardKey, name: 'index_2'}]}));

// First run of the command, should succeed, check that the metadata changed.
assert.commandWorked(runConfigsvrCommitRefineCollectionShardKey(
    st, ns, initialCollectionMetadata.timestamp, Timestamp(), newShardKey));

const finalCollectionMetadata = st.s.getCollection('config.collections').findOne({_id: ns});

assert.neq(initialCollectionMetadata.key, finalCollectionMetadata.key);
assert.neq(initialCollectionMetadata.timestamp, finalCollectionMetadata.timestamp);
assert.neq(initialCollectionMetadata.lastmodEpoch, finalCollectionMetadata.lastmodEpoch);

// Idempotency check, the command should succeed but no change should happen.
assert.commandWorked(runConfigsvrCommitRefineCollectionShardKey(
    st, ns, initialCollectionMetadata.timestamp, finalCollectionMetadata.timestamp, newShardKey));

// Ensure causality by making sure all nodes have the commit replicated.
st.configRS.awaitLastOpCommitted(1 * 60 * 1000);

const noopCollectionMetadata = st.s.getCollection('config.collections').findOne({_id: ns});

assert.eq(noopCollectionMetadata.key, finalCollectionMetadata.key);
assert.eq(noopCollectionMetadata.timestamp, finalCollectionMetadata.timestamp);
assert.eq(noopCollectionMetadata.lastmodEpoch, finalCollectionMetadata.lastmodEpoch);

// This should fail, the newTimestamp must match with the first newTimestamp committed, just like
// the oldTimestamp does.
assert.commandFailedWithCode(
    runConfigsvrCommitRefineCollectionShardKey(
        st, ns, initialCollectionMetadata.timestamp, Timestamp(), newShardKey),
    7648608);

// This should also fail, oldTimestamp will not match.
assert.commandFailedWithCode(
    runConfigsvrCommitRefineCollectionShardKey(st, ns, Timestamp(), Timestamp(), newShardKey),
    7648608);

assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns}));

st.stop();
})();
