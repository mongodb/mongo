/*
 * Basic tests for _shardsvrGetStatsForBalancing
 *
 * @tags: [
 *    requires_fcv_60,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");  // for 'configureFailPoint()'

const rangeDeleterBatchSize = 128;

const st = new ShardingTest({
    shards: 2,
    other: {shardOptions: {setParameter: {rangeDeleterBatchSize: rangeDeleterBatchSize}}}
});

function getCollSizeBytes(ns, node, optUUID) {
    let res;
    let collections = [{ns: ns}];
    if (optUUID) {
        collections[0].UUID = optUUID;
    }
    assert.soon(() => {
        res = assert.commandWorkedOrFailedWithCode(
            node.adminCommand(
                {_shardsvrGetStatsForBalancing: 1, collections: collections, scaleFactor: 1}),
            [ErrorCodes.NotYetInitialized]);
        return res.ok;
    });

    return res['stats'][0]['collSize'];
}
const kSizeSingleDocBytes = 18;
// work on non-existing collections
st.forEachConnection((shard) => {
    assert.eq(0, getCollSizeBytes("db.not_exists", shard.rs.getPrimary()));
});

const dbName = 'db';
const db = st.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

{
    // work on unsharded collections
    let coll = db['unsharded1'];
    assert.writeOK(coll.insert({_id: 1}));
    assert.eq(kSizeSingleDocBytes, getCollSizeBytes(coll.getFullName(), st.shard0.rs.getPrimary()));
    assert.eq(0, getCollSizeBytes(coll.getFullName(), st.shard1.rs.getPrimary()));
}

// work on sharded collections
let coll = db['sharded1'];
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
st.forEachConnection((shard) => {
    assert.eq(0, getCollSizeBytes(coll.getFullName(), shard.rs.getPrimary()));
});

const numDocs = 1000;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Create two chunks
assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: numDocs / 2}}));

// Check the data size is correct before the chunk is moved
assert.eq(kSizeSingleDocBytes * numDocs,
          getCollSizeBytes(coll.getFullName(), st.shard0.rs.getPrimary()));
assert.eq(0, getCollSizeBytes(coll.getFullName(), st.shard1.rs.getPrimary()));

{
    // Check that optional collection's UUID is handled correctly:
    // - Return correct data size if UUID matches
    // - Return `0` data size if UUID doesn't match
    let config = st.configRS.getPrimary().getDB("config");
    let collectionUUID = config.collections.findOne({_id: coll.getFullName()}).uuid;
    assert.eq(kSizeSingleDocBytes * numDocs,
              getCollSizeBytes(coll.getFullName(), st.shard0.rs.getPrimary(), collectionUUID));
    assert.eq(0, getCollSizeBytes(coll.getFullName(), st.shard0.rs.getPrimary(), UUID()));
}

// Pause before first range deletion task
let beforeDeletionFailpoint = configureFailPoint(st.shard0, "hangBeforeDoingDeletion");
let afterDeletionFailpoint = configureFailPoint(st.shard0, "hangAfterDoingDeletion");
assert.commandWorked(db.adminCommand(
    {moveChunk: coll.getFullName(), find: {_id: (numDocs / 2)}, to: st.shard1.shardName}));

const expectedShardSizeBytes = kSizeSingleDocBytes * (numDocs / 2);
st.forEachConnection((shard) => {
    assert.eq(expectedShardSizeBytes, getCollSizeBytes(coll.getFullName(), shard.rs.getPrimary()));
});

// Check that dataSize is always correct during range deletions
const numBatches = (numDocs / 2) / rangeDeleterBatchSize;
for (let i = 0; i < numBatches; i++) {
    // Wait for failpoint and check num orphans
    beforeDeletionFailpoint.wait();
    st.forEachConnection((shard) => {
        assert.eq(expectedShardSizeBytes,
                  getCollSizeBytes(coll.getFullName(), shard.rs.getPrimary()));
    });
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
