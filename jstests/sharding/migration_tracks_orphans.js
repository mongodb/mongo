/**
 * Tests that the recipient in a migration correctly tracks the number of orphans during cloning and
 * transfer mods. The count of orphan documents should be persisted in the range deletion document
 * in config.rangeDeletions.
 *
 * @tags: [
 *  requires_fcv_60,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const st = new ShardingTest({shards: 2});

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

// Pause after bulk clone and check number of orphans is equal to numDocs
let bulkCloneFailpoint = configureFailPoint(st.shard1, "migrateThreadHangAtStep4");
const awaitResult = startParallelShell(
    funWithArgs(function(nss, toShardName) {
        assert.commandWorked(db.adminCommand({moveChunk: nss, find: {_id: 0}, to: toShardName}));
    }, nss, st.shard1.shardName), st.s.port);

// Assert that the range deletion document is present and has the correct number of orphans.
bulkCloneFailpoint.wait();
assertOrphanCountIsCorrect(st.shard1, nss, numDocs);
assertOrphanCountIsCorrect(st.shard0, nss, 0);

// Pause after transfer mods and check number of orphans has changed correctly.
let transferModsFailpoint = configureFailPoint(st.shard1, "migrateThreadHangAtStep5");

// Perform some upserts and deletes to change the number of orphans on the recipient.
let bulkMods = coll.initializeUnorderedBulkOp();
const numUpserts = 50;
for (let i = 0; i < numUpserts; i++) {
    let key = numDocs + i;
    bulkMods.find({_id: key}).upsert().update({$set: {_id: key}});
}
const numDeletes = 25;
for (let i = 0; i < numDeletes; i++) {
    bulkMods.find({_id: i}).removeOne();
}
// Perform some updates that shouldn't change the number of orphans.
const numUpdates = 10;
for (let i = 0; i < numUpdates; i++) {
    let key = numDeletes + i;
    bulkMods.find({_id: key}).update({$set: {x: key}});
}
assert.commandWorked(bulkMods.execute());

// Assert that the number of orphans is still correct.
bulkCloneFailpoint.off();
transferModsFailpoint.wait();
const updatedOrphanCount = numDocs + numUpserts - numDeletes;
assertOrphanCountIsCorrect(st.shard1, nss, updatedOrphanCount);
assertOrphanCountIsCorrect(st.shard0, nss, 0);

// Allow migration to finish but stop right after updating range deletion documents
let migrationCommittedFailpoint =
    configureFailPoint(st.shard0, "hangBeforeForgettingMigrationAfterCommitDecision");
transferModsFailpoint.off();
migrationCommittedFailpoint.wait();
assertOrphanCountIsCorrect(st.shard0, nss, updatedOrphanCount);
assert.eq(0, st.shard1.getDB("config").getCollection("rangeDeletions").find().itcount());

// Allow migration to fully complete
migrationCommittedFailpoint.off();
awaitResult();

st.stop();
})();
