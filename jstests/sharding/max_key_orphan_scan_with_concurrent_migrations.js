/**
 * MaxKey orphan one-shot inventory scan must not block concurrent migrations and migrations
 * must not corrupt the scan's persisted result.
 * hangDuringMaxKeyOrphanScan parks the scan mid-detection: after it pre-checks a collection for a
 * MaxKey document, before its authoritative re-check. With the scan parked on an {a: MaxKey} document
 * shard0 owns, the test moves the global-max chunk off shard0 (_waitForDelete removes the document).
 * The moveChunk completing while parked proves the scan doesn't block it; the final foundMaxKey=false
 * proves the re-check observed the removal.
 *
 * @tags: [
 *  featureFlagMaxKeyDetection,
 *  multiversion_incompatible,
 *  does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

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

// Migrated in parallel to show multiple in-flight migrations aren't blocked. No MaxKey document, so
// the scan never pauses on it.
const parallelCollName = "parallel_coll";
const parallelNs = `${dbName}.${parallelCollName}`;
assert.commandWorked(adminDB.runCommand({shardCollection: parallelNs, key: {a: 1}}));
assert.commandWorked(adminDB.runCommand({split: parallelNs, middle: {a: 0}}));
for (let i = -5; i < 5; ++i) {
    assert.commandWorked(testDB[parallelCollName].insert({a: i}));
}

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

jsTest.log.info("Moving the global-max chunk off shard0 while the orphan scan is parked");
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: candidateNs,
        bounds: [{a: 0}, {a: MaxKey}],
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);
assert.eq(
    0,
    st.rs0.getPrimary().getDB(dbName).getCollection(candidateCollName).find({a: MaxKey}).itcount(),
    "The migration's range deletion should have removed the global-max document from shard0",
);

jsTest.log.info("Running a second migration in parallel while the scan is still parked");
const parallelMove = startParallelShell(
    funWithArgs(
        function (ns, toShard) {
            assert.commandWorked(
                db.getSiblingDB("admin").runCommand({
                    moveChunk: ns,
                    find: {a: 1},
                    to: toShard,
                    _waitForDelete: true,
                }),
            );
        },
        parallelNs,
        st.shard1.shardName,
    ),
    mongos.port,
);

assert.eq(null, readScanState(), "Scan must remain parked while the migrations are in flight");

hangFp.off();

assert.soon(
    () => {
        const doc = readScanState();
        return doc !== null && doc.scanCompletedAt !== undefined;
    },
    "Expected the orphan scan to complete after the failpoint is disabled",
    60 * 1000,
);

parallelMove(); // join the parallel shell; verifies the second moveChunk succeeded

const finalState = readScanState();
assert.eq(
    false,
    finalState.foundMaxKey,
    "Scan's authoritative re-check must observe the concurrently-removed candidate and not flag it",
    {finalState},
);

st.stop();
