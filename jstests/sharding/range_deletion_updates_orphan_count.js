/**
 * Tests that the range deleter updates the number of orphans from a migration with every deleted
 * orphan batch.
 *
 * @tags: [
 *  requires_fcv_60,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const rangeDeleterBatchSize = 128;

const st = new ShardingTest({
    shards: 2,
    other: {
        shardOptions: {setParameter: {rangeDeleterBatchSize: rangeDeleterBatchSize}},
    }
});

// Setup database and collection for test
const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = db['test'];
const nss = coll.getFullName();
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

function assertOrphanCountIsCorrect(conn, ns, numOrphans) {
    const rangeDeletionDoc =
        conn.getDB("config").getCollection("rangeDeletions").findOne({nss: ns});
    assert.neq(null,
               rangeDeletionDoc,
               "did not find document for namespace " + ns +
                   ", contents of config.rangeDeletions on " + conn + ": " +
                   tojson(conn.getDB("config").getCollection("rangeDeletions").find().toArray()));
    assert.eq(numOrphans,
              rangeDeletionDoc.numOrphanDocs,
              "Incorrect count of orphaned documents in config.rangeDeletions on " + conn +
                  ": expected " + numOrphans +
                  " orphaned documents but found range deletion document " +
                  tojson(rangeDeletionDoc));
}

// Insert some docs into the collection.
const numDocs = 1000;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Pause before first range deletion task
let beforeDeletionFailpoint = configureFailPoint(st.shard0, "hangBeforeDoingDeletion");
let afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
assert.commandWorked(db.adminCommand({moveChunk: nss, find: {_id: 0}, to: st.shard1.shardName}));

// Check the batches are deleted correctly
const numBatches = numDocs / rangeDeleterBatchSize;
for (let i = 0; i < numBatches; i++) {
    // Wait for failpoint and check num orphans
    beforeDeletionFailpoint.wait();
    assertOrphanCountIsCorrect(st.shard0, nss, numDocs - rangeDeleterBatchSize * i);
    // Unset and reset failpoint without allowing any batches deleted in the meantime
    afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
    beforeDeletionFailpoint.off();
    afterDeletionFailpoint.wait();
    beforeDeletionFailpoint = configureFailPoint(st.shard0, "hangBeforeDoingDeletion");
    afterDeletionFailpoint.off();
}
beforeDeletionFailpoint.off();

st.stop();
})();
