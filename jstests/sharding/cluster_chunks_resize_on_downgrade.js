/**
 * Tests that chunks are re-split upon downgrade from v6.0.
 *
 * @tags: [
 *  requires_fcv_61,
 * ]
 */

(function() {
'use strict';
load("jstests/sharding/libs/find_chunks_util.js");

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1},
    other: {enableBalancer: false, enableAutoSplit: false, chunkSize: 1}
});

// Setup database and collection for test
const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = db['test'];
const nss = coll.getFullName();
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

const bigString = 'X'.repeat(1024 * 1024);  // 1MB

// Insert 10MB of docs into the collection.
const numDocs = 10;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i, s: bigString});
}
assert.commandWorked(bulk.execute());

const numChunksBeforeDowngrade = findChunksUtil.countChunksForNs(st.config, nss);

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

const numChunksAfterDowngrade = findChunksUtil.countChunksForNs(st.config, nss);
jsTest.log("Number of chunks: before downgrade = " + numChunksBeforeDowngrade +
           ", after = " + numChunksAfterDowngrade);
assert.eq(10, numChunksAfterDowngrade);

st.stop();
})();
