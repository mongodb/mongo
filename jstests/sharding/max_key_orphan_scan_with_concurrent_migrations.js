/**
 * A chunk migration racing the MaxKey orphan one-shot inventory scan must not corrupt the scan's
 * persisted result. hangDuringMaxKeyOrphanScan parks the scan after it pre-checks a collection for a
 * MaxKey document, before its authoritative re-check. Once released, the scan races a migration of
 * the global-max chunk off shard0. The MigrationBlockingGuard serializes them, so the migration
 * either wins (removes the document) or is rejected with ConflictingOperationInProgress; either way
 * the re-check reports foundMaxKey=false.
 *
 * @tags: [
 *  featureFlagMaxKeyDetection,
 *  requires_fcv_90,
 *  does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

for (const rs of [st.rs0, st.rs1]) {
    for (const node of rs.nodes) {
        assert.commandWorkedOrFailedWithCode(
            node.adminCommand({setParameter: 1, skipRangeDeletionForMaxKeyChunks: false}),
            ErrorCodes.InvalidOptions,
        );
    }
}

const dbName = jsTestName();
const mongos = st.s0;
const adminDB = mongos.getDB("admin");
const testDB = mongos.getDB(dbName);

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// shard0 owns the global-max chunk holding {a: MaxKey}, so the scan's pre-check finds it.
const candidateCollName = "candidate_coll";
const candidateNs = `${dbName}.${candidateCollName}`;
assert.commandWorked(adminDB.runCommand({shardCollection: candidateNs, key: {a: 1}}));
assert.commandWorked(adminDB.runCommand({split: candidateNs, middle: {a: 0}}));
assert.commandWorked(testDB[candidateCollName].insert({a: MaxKey}));
assert.commandWorked(testDB[candidateCollName].insert({a: -1}));

const scanStateId = "scanState";
function readScanState() {
    return st.rs0.getPrimary().getDB("config").getCollection("maxKeyOrphanScanState").findOne({
        _id: scanStateId,
    });
}

// Clear any state doc the initial stepup may have already persisted so the next stepup re-runs the scan and
// parks on the failpoint we're about to arm.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB("config")
        .runCommand({
            delete: "maxKeyOrphanScanState",
            deletes: [{q: {_id: scanStateId}, limit: 0}],
            writeConcern: {w: "majority"},
        }),
);
st.rs0.awaitReplication();

// Arm hangDuringMaxKeyOrphanScan on the next primary, then step it up. The scan finds the {a: MaxKey}
// candidate and parks after the pre-check but before the migration-blocking guard.
const nextPrimary = st.rs0.getSecondary();
const hangFp = configureFailPoint(nextPrimary, "hangDuringMaxKeyOrphanScan");
st.rs0.stepUp(nextPrimary);
st.rs0.waitForPrimary();
hangFp.wait({maxTimeMS: kDefaultWaitForFailPointTimeout});

assert.eq(null, readScanState(), "Scan must not have persisted the state doc while paused");

hangFp.off();

jsTest.log.info("Racing a migration of the global-max chunk against the resuming orphan scan");
const moveRes = adminDB.runCommand({
    moveChunk: candidateNs,
    bounds: [{a: 0}, {a: MaxKey}],
    to: st.shard1.shardName,
    _waitForDelete: true,
});
assert(
    moveRes.ok === 1 || moveRes.code === ErrorCodes.ConflictingOperationInProgress,
    "Migration must either succeed or be rejected by the scan's guard",
    {moveRes},
);

assert.soon(
    () => {
        const doc = readScanState();
        return doc !== null && doc.scanCompletedAt !== undefined;
    },
    "Expected the orphan scan to complete after the failpoint is disabled",
    60 * 1000,
);

const finalState = readScanState();
assert.eq(
    false,
    finalState.foundMaxKey,
    "Scan must not flag: the doc was removed by the migration or is still legitimately owned by shard0",
    {finalState},
);

st.stop();
