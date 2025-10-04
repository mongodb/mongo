/**
 * This test reproduces the error reported in HELP-22995. It creates a jumbo chunk with documents
 * that are close to the 16MB document size limit to force the batching code in move chunk to
 * consider adding them together in a batch. It ensures that the proper document size is considered
 * and that we can still migrate when calling removeShard.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   multiversion_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const dbName = "test";
const collName = "user";
const ns = dbName + "." + collName;
const shardKeys = [-1, 1];

// This number is chosen so that the chunks are considered 'large' as defined by
// the MigrationChunkClonerSource class. Currently, that class considers chunks containing
// more than the following number of documents as 'large':
//    (13/10) * MaxChunkSize / avgRecSize (MaxChunkSize is 64MB by default)
const numDocs = 10;

// Size is slightly under the 16MB document size limit. This ensures that any two documents must be
// be in separate batches when cloning.
const bigDocSize = 16 * 1024 * 1024 - 4096;
const bigDocPayload = "x".repeat(bigDocSize);

let st = new ShardingTest({shards: 2});
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

jsTest.log("Sharding collection with one chunk on each shard.");
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

function assertDocsExist(shardKeys, numDocs, payloadSize) {
    shardKeys.forEach((key) => {
        for (let i = 0; i < numDocs; i++) {
            let db = st.rs0.getPrimary().getDB(dbName);
            let query = {x: key, seq: i};
            let doc = db.getCollection(collName).findOne(query);
            assert(doc);
            let payload = doc.data;
            assert.eq(
                payload.length,
                payloadSize,
                tojson(query) + " does not have the expected payload length of " + payloadSize + " bytes",
            );
        }
    });
}

jsTest.log("Insert " + numDocs + " documents with " + bigDocSize + " bytes each.");
shardKeys.forEach((key) => {
    for (let i = 0; i < numDocs; i++) {
        let doc = {x: key, seq: i, data: bigDocPayload};
        assert.commandWorked(st.s.getCollection(ns).insert(doc));
    }
});

// Start balancer to migrate chunks from the removed shard.
assert.commandWorked(
    st.s.getDB("config").settings.update({_id: "balancer"}, {$set: {attemptToBalanceJumboChunks: true}}, true),
);
st.startBalancer();

removeShard(st, st.shard1.shardName);
assertDocsExist(shardKeys, numDocs, bigDocSize);

st.stop();
