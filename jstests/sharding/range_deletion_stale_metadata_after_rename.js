/**
 * SERVER-114326 reproduction: a RangeDeletionTask must not observe stale
 * CollectionShardingRuntime metadata while a rename is in flight.
 *
 * The rename coordinator commits in two oplog entries:
 *   (1) the rewrite of every pending RangeDeletionTask's collectionUuid from
 *       the pre-rename UUID to the post-rename UUID, and
 *   (2) the FilteringMetadataClearer pass that drops the cached CSR metadata
 *       entry for the renamed namespace.
 *
 * Empirically the gap between (1) and (2) has been measured at ~300 ms.
 * Within that window RangeDeleterServiceOpObserver::onUpdate fires on (1)
 * and reads a task stamped with the new UUID while the metadataTracker held
 * by the CSR is still pinned to the old UUID. SERVER-113667 short-circuits
 * the invalidateRangePreservers call when the UUIDs disagree; this test
 * pins the underlying ordering invariant by driving the window directly via
 * failpoints and asserting that no mismatched invalidation ever fires.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_sharding,
 *   requires_majority_read_concern,
 *   featureFlagRangeDeleterRenameSafety,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const dbName = "test";
const sourceCollName = "rangedel_rename_source";
const targetCollName = "rangedel_rename_target";
const sourceNss = dbName + "." + sourceCollName;
const targetNss = dbName + "." + targetCollName;

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    other: {
        configOptions: {setParameter: {orphanCleanupDelaySecs: 0}},
        rsOptions: {setParameter: {orphanCleanupDelaySecs: 0, rangeDeleterBatchSize: 16}},
    },
});

const mongos = st.s;
const shard0 = st.shard0;
const shard1 = st.shard1;
const shard0Primary = st.rs0.getPrimary();

// Set up the source collection: sharded by {x: 1}, split at {x: 0}, one chunk
// on each shard so a moveChunk creates a RangeDeletionTask on shard0.
assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: sourceNss, key: {x: 1}}));
assert.commandWorked(mongos.adminCommand({split: sourceNss, middle: {x: 0}}));

const sourceColl = mongos.getCollection(sourceNss);
let bulk = sourceColl.initializeUnorderedBulkOp();
for (let i = -200; i < 200; i++) {
    bulk.insert({x: i, payload: "abcdefghij"});
}
assert.commandWorked(bulk.execute());

// Suspend range deletion on shard0 so the RangeDeletionTask remains pending
// after the moveChunk completes. This guarantees there is a task in
// config.rangeDeletions whose collectionUuid the rename coordinator will
// later rewrite.
const suspendDeletion = configureFailPoint(shard0Primary, "suspendRangeDeletion");

assert.commandWorked(mongos.adminCommand({
    moveChunk: sourceNss,
    find: {x: 100},
    to: shard1.shardName,
    _waitForDelete: false,
}));

const rangeDeletionsBeforeRename =
    shard0Primary.getDB("config").getCollection("rangeDeletions").find({nss: sourceNss}).toArray();
assert.gte(rangeDeletionsBeforeRename.length, 1,
           "expected at least one pending RangeDeletionTask on shard0 before rename");
const preRenameCollUuid = rangeDeletionsBeforeRename[0].collectionUuid;
jsTest.log("Pre-rename RangeDeletionTask UUID on shard0: " + tojson(preRenameCollUuid));

// Drop the target so the rename has a clear destination.
assert.commandWorked(mongos.getDB(dbName).runCommand({drop: targetCollName}));

// Open the 300 ms TOCTOU window. hangBeforeFilteringMetadataClearer pauses
// after the rename coordinator has committed the rangeDeletions UUID rewrite
// but before the CSR metadata clear has been applied. This is the production
// window in which onUpdate may observe a (newUUID, oldUUID) pair.
const hangBeforeMetadataClear =
    configureFailPoint(shard0Primary, "hangBeforeFilteringMetadataClearerOnRename");

const renameShell = startParallelShell(funWithArgs(function(dbName, fromColl, toColl) {
    assert.commandWorked(db.getSiblingDB("admin").runCommand({
        renameCollection: dbName + "." + fromColl,
        to: dbName + "." + toColl,
        dropTarget: true,
    }));
}, dbName, sourceCollName, targetCollName), mongos.port);

hangBeforeMetadataClear.wait();
jsTest.log("Reached pre-FilteringMetadataClearer hang; UUID rewrite has committed.");

// Within the window: confirm the RangeDeletionTask now carries the new UUID
// while the CSR for the original namespace is either still cached against
// the old UUID or has not yet been refreshed.
const rangeDeletionsInWindow =
    shard0Primary.getDB("config").getCollection("rangeDeletions").find({}).toArray();
assert.gte(rangeDeletionsInWindow.length, 1,
           "expected RangeDeletionTask still present inside the TOCTOU window");
const inWindowCollUuid = rangeDeletionsInWindow[0].collectionUuid;
jsTest.log("In-window RangeDeletionTask UUID on shard0: " + tojson(inWindowCollUuid));
assert.neq(bsonWoCompare(inWindowCollUuid, preRenameCollUuid), 0,
           "rename coordinator should have rewritten the RangeDeletionTask UUID by now");

// Drive a range-deleter pass while the window is open. The fix (paired with
// the SERVER-113667 quick-fix) is to refuse invalidation when task.uuid
// disagrees with metadata.uuid, and to defer task processing until the
// metadata has been refreshed. Either way, no mismatched invalidation
// should be logged.
const beforeInvalidationCount = (function () {
    const log = assert.commandWorked(shard0Primary.adminCommand({getLog: "global"})).log;
    return log.filter(line => line.indexOf("invalidateRangePreservers") !== -1
                              && line.indexOf("UUID mismatch") !== -1).length;
})();

suspendDeletion.off();

// Hold the window open long enough for the deleter to observe the in-flight
// state. 300 ms matches the empirical gap in the ticket; we wait 1 s to be
// generous against slow CI hosts.
sleep(1000);

// Close the window: let FilteringMetadataClearer run.
hangBeforeMetadataClear.off();
renameShell();

// Validate the headline invariant: the range deleter never invalidated
// preservers with mismatched UUIDs.
const afterInvalidationCount = (function () {
    const log = assert.commandWorked(shard0Primary.adminCommand({getLog: "global"})).log;
    return log.filter(line => line.indexOf("invalidateRangePreservers") !== -1
                              && line.indexOf("UUID mismatch") !== -1).length;
})();
assert.eq(afterInvalidationCount, beforeInvalidationCount,
          "RangeDeleter must not invalidate preservers with mismatched UUIDs across rename");

// Validate post-rename steady state: the renamed namespace is reachable,
// the source namespace is gone, and the RangeDeletionTask either drained or
// carries the post-rename UUID consistently with the cached metadata.
assert.eq(mongos.getDB(dbName).getCollectionInfos({name: sourceCollName}).length, 0,
          "source collection should no longer exist after rename");
assert.eq(mongos.getDB(dbName).getCollectionInfos({name: targetCollName}).length, 1,
          "target collection should exist after rename");

const finalRangeDeletions =
    shard0Primary.getDB("config").getCollection("rangeDeletions").find({}).toArray();
for (const doc of finalRangeDeletions) {
    assert.neq(bsonWoCompare(doc.collectionUuid, preRenameCollUuid), 0,
               "no RangeDeletionTask should still carry the pre-rename UUID after rename: "
               + tojson(doc));
}

st.stop();
