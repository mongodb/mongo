/**
 * Tests the standalone MaxKey orphan detector that runs asynchronously on each shard-primary stepup
 * and persists its outcome to config.maxKeyOrphanScanState.
 *
 * Cases:
 *   1. Clean cluster: foundMaxKey=false; a second stepup must not rewrite the doc (one-shot guard).
 *   2. Non-findings in a single sweep: an owned all-MaxKey doc, a hashed shard key, and an
 *      all-MaxKey doc covered by a pending range-deletion task. None are flagged.
 *   3. Already-orphan single-field shard key: flagged, WARNING fires, alertEmitted=true.
 *   4. Already-orphan compound shard key (all fields MaxKey): flagged.
 *   5. Already-orphan compound shard key with only the leading field MaxKey (e.g. {a: MaxKey,
 *      b: 10}): flagged. Guards against matching only all-MaxKey shard keys.
 *   6. Owned partial-MaxKey doc behind a split inside the leading-MaxKey region: not flagged,
 *      because this shard owns the chunk holding the document.
 *   7. Partial-MaxKey orphan covered by an ordinary (non-global-max) range-deletion task: not
 *      flagged, because the pending cleanup will delete it.
 *   8. Orphan detected through a wider index than the shard key (shard key {a: 1} backed only by a
 *      compound {a: 1, b: 1} index): flagged. Exercises reading shard-key values from a wider index
 *      and discarding the extra trailing field to recover the shard key.
 *   9. Empty sharded collection: not flagged (the backward index scan finds nothing).
 *  10. Failover before persist: pause the scan just before it upserts the state doc, stepdown to
 *      interrupt it, disable the failpoint, and confirm the next primary re-runs the scan to
 *      completion (one-shot re-run after an interrupted sweep).
 *
 * @tags: [
 *  featureFlagMaxKeyDetection,
 *  multiversion_incompatible,
 *  does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Cases 3, 4, and 5 intentionally leave MaxKey orphans on shard0, so suppress the teardown
// orphan check.
TestData.skipCheckOrphans = true;

const scanStateId = "scanState";
const warningLogId = 12799008;

function readScanState(shard) {
    return shard.getDB("config").getCollection("maxKeyOrphanScanState").findOne({_id: scanStateId});
}

function stepDownAndUp(rs) {
    const newPrimary = rs.getSecondary();
    rs.stepUp(newPrimary);
    rs.waitForPrimary();
}

function readOrphanScanStats(rs) {
    return assert.commandWorked(rs.getPrimary().adminCommand({serverStatus: 1})).shardingStatistics;
}

// Asserts the FTDC counters reflect a completed scan with the given outcome. Cases wait on the state
// doc for completion (scanCompletedAt) and assert the classification here, so it is checked once.
function assertOrphanScanStats(rs, {foundMaxKey, alertEmitted}, message) {
    let stats;
    assert.soon(
        () => {
            stats = readOrphanScanStats(rs);
            return (
                stats.maxKeyOrphanScanComplete == 1 &&
                stats.maxKeyOrphanScanFoundMaxKey == (foundMaxKey ? 1 : 0) &&
                stats.maxKeyOrphanScanAlertEmitted == (alertEmitted ? 1 : 0)
            );
        },
        () => `${message}; got ${tojson(stats)}`,
    );
}

function configureFailCommandAllConfigNodes(rs, cmd) {
    rs.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(cmd));
    });
}

// Always fetch a collection handle bound to the *current* primary. Caching a handle across a
// stepup leaves it pointing at the old (now secondary) node, so subsequent writes fail with
// NotWritablePrimary.
function rangeDeletionsOnPrimary(rs) {
    return rs.getPrimary().getDB("config").getCollection("rangeDeletions");
}

// Polls the current primary for the scan-state doc within 'timeoutMs'.
function awaitScanStateDoc(rs, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        try {
            const doc = readScanState(rs.getPrimary());
            if (doc !== null) {
                return doc;
            }
        } catch (e) {
            jsTest.log.info("Transient error reading scan state; will retry", {
                error: e.toString(),
            });
        }
        sleep(250);
    }
    return null;
}

// Steps the donor up and polls for the state doc to satisfy 'predicate'. The scan's persist can
// fail transiently with NotWritablePrimary in topologies that churn primary status during stepup
// (notably config-shard during cluster init); the scan swallows it and leaves the state doc absent,
// so the test re-steps until the next stable primary completes the scan. If the doc is present but
// the predicate doesn't match, we fail loudly instead of retrying because that is a real assertion
// failure, not a stepup race.
function stepUpAndAwaitScanState(rs, predicate, message, maxAttempts = 10) {
    for (let attempt = 1; attempt <= maxAttempts; attempt++) {
        stepDownAndUp(rs);
        const doc = awaitScanStateDoc(rs, 15 * 1000);
        if (doc !== null) {
            assert(predicate(doc), `${message}; got: ${tojson(doc)}`);
            return doc;
        }
        jsTest.log.info(
            `Scan state doc not observed on stepup attempt ${attempt}/${maxAttempts}; retrying`,
        );
    }
    assert(false, `${message}; state doc never appeared after ${maxAttempts} stepups`);
}

// The scan is one-shot: once any primary persists scanCompletedAt, subsequent stepups
// short-circuit. Later cases need a fresh enumeration, so they delete the state doc with
// majority write concern before the next stepup so whichever node is promoted observes it
// absent.
function clearScanStateAndWaitMajority(rs) {
    const primary = rs.getPrimary();
    assert.commandWorked(
        primary.getDB("config").runCommand({
            delete: "maxKeyOrphanScanState",
            deletes: [{q: {_id: scanStateId}, limit: 0}],
            writeConcern: {w: "majority"},
        }),
    );
    rs.awaitReplication();
}

// Raise orphanCleanupDelaySecs to not delete MaxKey orphans for testing.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2},
    rsOptions: {setParameter: {orphanCleanupDelaySecs: 600}},
});

const dbName = jsTestName();
const mongos = st.s0;
const adminDB = mongos.getDB("admin");
const testDB = mongos.getDB(dbName);

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

st.rs0.nodes.forEach((node) =>
    assert.commandWorked(
        node.adminCommand({setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 2}}}),
    ),
);

function resetClusterState(collNames) {
    for (const collName of collNames) {
        assert.commandWorked(testDB.runCommand({drop: collName}));
    }
    assert.commandWorked(
        st.rs0.getPrimary().getDB("config").getCollection("rangeDeletions").deleteMany({}),
    );
    st.rs0.awaitReplication();
}

// --- Case 1: clean cluster ----------------------------------------------------------------------
jsTest.log.info("Case 1: clean cluster, expect foundMaxKey=false after stepup");

const cleanState = stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should persist scanCompletedAt on a clean cluster",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: false, alertEmitted: false},
    "maxKeyOrphanScan stats must reflect clean scan outcome",
);

// One-shot guard: a second stepup with scanCompletedAt persisted must not rewrite the doc.
// Assert byte-equality (including timestamps) to confirm no upsert ran.
jsTest.log.info("Case 1 follow-up: second stepup must short-circuit");
stepDownAndUp(st.rs0);
// Wait for the scan's short-circuit log (this term) -- proof it ran and skipped -- then assert it
// left the doc untouched.
const guardTerm = assert.commandWorked(
    st.rs0.getPrimary().adminCommand({replSetGetStatus: 1}),
).term;
checkLog.containsJson(st.rs0.getPrimary(), 12799006, {term: Number(guardTerm)});
assert.docEq(
    cleanState,
    readScanState(st.rs0.getPrimary()),
    "State doc must be unchanged after re-stepup once scanCompletedAt is persisted",
);
// The short-circuit path re-publishes the prior outcome to FTDC (distinct from the initial publish).
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: false, alertEmitted: false},
    "short-circuit must republish the prior clean outcome to FTDC stats",
);

// --- Case 2: non-findings (owned all-MaxKey doc, hashed key, range-deletion-covered doc) --------
jsTest.log.info(
    "Case 2: owned all-MaxKey doc, hashed shard key, and a covered doc must not be flagged",
);

// (a) Owned all-MaxKey doc: shard0 keeps the (only) chunk so it legitimately owns {a: MaxKey}.
const ownedColl = "owned_coll";
const ownedNs = `${dbName}.${ownedColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: ownedNs, key: {a: 1}}));
assert.commandWorked(testDB[ownedColl].insert({a: MaxKey, payload: "owned-maxkey-doc"}));

// (b) Hashed shard key: detector skips hashed collections outright.
const hashedColl = "hashed_coll";
const hashedNs = `${dbName}.${hashedColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: hashedNs, key: {a: "hashed"}}));
assert.commandWorked(testDB[hashedColl].insert({a: 1, payload: "hashed-doc"}));

// (c) Covered doc: a MaxKey doc whose chunk moved away but is still referenced by a pending
// MaxKey-bounded range-deletion task on shard0.
const coveredColl = "covered_coll";
const coveredNs = `${dbName}.${coveredColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: coveredNs, key: {a: 1}}));
assert.commandWorked(testDB[coveredColl].insert({a: MaxKey, payload: "covered-maxkey-doc"}));
assert.commandWorked(testDB[coveredColl].insert({a: 50, payload: "normal-doc"}));
assert.commandWorked(adminDB.runCommand({split: coveredNs, middle: {a: 0}}));
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: coveredNs,
        find: {a: 50},
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);
const coveredUUID = testDB.getCollectionInfos({name: coveredColl})[0].info.uuid;
assert.soon(
    () =>
        st.rs0
            .getPrimary()
            .getDB("config")
            .getCollection("rangeDeletions")
            .findOne({collectionUuid: coveredUUID, "range.max.a": MaxKey}) !== null,
    "Expected a pending MaxKey-bounded range-deletion task on the donor for the covered collection",
);

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete on a cluster with only non-findings",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: false, alertEmitted: false},
    "Detector must not flag an owned all-MaxKey doc, a hashed shard key, or a covered doc",
);

resetClusterState([ownedColl, hashedColl, coveredColl]);

// --- Case 3: already-orphan MaxKey doc with no covering range-deletion task ---------------------
jsTest.log.info(
    "Case 3: already-orphan single-field MaxKey doc must be flagged and WARNING must fire",
);

const orphanColl = "orphan_coll";
const orphanNs = `${dbName}.${orphanColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: orphanNs, key: {a: 1}}));
assert.commandWorked(testDB[orphanColl].insert({a: MaxKey, payload: "orphan-maxkey-doc"}));
assert.commandWorked(testDB[orphanColl].insert({a: 50, payload: "normal-doc"}));
assert.commandWorked(adminDB.runCommand({split: orphanNs, middle: {a: 0}}));

assert.commandWorked(
    adminDB.runCommand({
        moveChunk: orphanNs,
        find: {a: 50},
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);

const orphanUUID = testDB.getCollectionInfos({name: orphanColl})[0].info.uuid;
assert.soon(
    () =>
        rangeDeletionsOnPrimary(st.rs0).findOne({
            collectionUuid: orphanUUID,
            "range.max.a": MaxKey,
        }) !== null,
    "Expected the pending MaxKey-bounded range-deletion task to appear on the donor",
);

// Delete every range-deletion task on shard0, simulating a past buggy task that already ran
// or was lost while the MaxKey doc remained on disk.
assert.commandWorked(rangeDeletionsOnPrimary(st.rs0).deleteMany({}));
st.rs0.awaitReplication();

// Direct shell connection bypasses orphan filtering, so the MaxKey doc is observable even
// though shard0 no longer owns the chunk.
assert.eq(
    1,
    st.rs0.getPrimary().getDB(dbName).getCollection(orphanColl).find({a: MaxKey}).itcount(),
    "Expected the MaxKey doc to remain on shard0 after wiping the range-deletion task",
);

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete after an already-orphan MaxKey doc is left on shard0",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: true, alertEmitted: true},
    "Detector should flag the already-orphan MaxKey doc and record alertEmitted=true",
);

checkLog.containsJson(st.rs0.getPrimary(), warningLogId);

resetClusterState([orphanColl]);

// --- Case 4: already-orphan compound shard key --------------------------------------------------
jsTest.log.info("Case 4: already-orphan compound shard-key MaxKey doc must be flagged");

const compoundColl = "compound_coll";
const compoundNs = `${dbName}.${compoundColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: compoundNs, key: {a: 1, b: 1}}));
assert.commandWorked(
    testDB[compoundColl].insert({a: MaxKey, b: MaxKey, payload: "compound-orphan-doc"}),
);
assert.commandWorked(testDB[compoundColl].insert({a: 50, b: 50, payload: "normal-doc"}));
assert.commandWorked(adminDB.runCommand({split: compoundNs, middle: {a: 0, b: 0}}));
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: compoundNs,
        find: {a: 50, b: 50},
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);

const compoundUUID = testDB.getCollectionInfos({name: compoundColl})[0].info.uuid;
assert.soon(
    () =>
        rangeDeletionsOnPrimary(st.rs0).findOne({
            collectionUuid: compoundUUID,
            "range.max.a": MaxKey,
        }) !== null,
    "Expected the pending compound MaxKey-bounded range-deletion task on the donor",
);
assert.commandWorked(rangeDeletionsOnPrimary(st.rs0).deleteMany({}));
st.rs0.awaitReplication();

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete after an already-orphan compound-shard-key MaxKey doc is left on shard0",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: true, alertEmitted: true},
    "Detector should flag the already-orphan compound-shard-key MaxKey doc",
);

resetClusterState([compoundColl]);

// --- Case 5: already-orphan compound shard key with only the leading field MaxKey ---------------
// {a: MaxKey, b: 10} lives in the global-max chunk but is not all-MaxKey, so this verifies detection
// keys off the leading field rather than requiring every field to be MaxKey.
jsTest.log.info("Case 5: already-orphan compound doc with a partial-MaxKey leading field");

const partialColl = "partial_maxkey_coll";
const partialNs = `${dbName}.${partialColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: partialNs, key: {a: 1, b: 1}}));
assert.commandWorked(testDB[partialColl].insert({a: MaxKey, b: 10, payload: "partial-orphan-doc"}));
assert.commandWorked(testDB[partialColl].insert({a: 50, b: 50, payload: "normal-doc"}));
assert.commandWorked(adminDB.runCommand({split: partialNs, middle: {a: 0, b: 0}}));
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: partialNs,
        find: {a: 50, b: 50},
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);

const partialUUID = testDB.getCollectionInfos({name: partialColl})[0].info.uuid;
assert.soon(
    () =>
        rangeDeletionsOnPrimary(st.rs0).findOne({
            collectionUuid: partialUUID,
            "range.max.a": MaxKey,
        }) !== null,
    "Expected the pending compound MaxKey-bounded range-deletion task on the donor",
);
assert.commandWorked(rangeDeletionsOnPrimary(st.rs0).deleteMany({}));
st.rs0.awaitReplication();

// Confirm the partial-MaxKey doc remains on shard0 and there is no all-MaxKey doc to fall back on.
assert.eq(
    1,
    st.rs0.getPrimary().getDB(dbName).getCollection(partialColl).find({a: MaxKey, b: 10}).itcount(),
    "Expected the partial-MaxKey doc to remain on shard0 after wiping the range-deletion task",
);

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete after a partial-MaxKey (leading field only) orphan is left on shard0",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: true, alertEmitted: true},
    "Detector should flag an orphan whose leading shard-key field is MaxKey but trailing field is not",
);

resetClusterState([partialColl]);

// --- Case 6: owned partial-MaxKey doc behind a split inside the leading-MaxKey region ------------
// A boundary at {a: MaxKey, b: 10} puts {a: MaxKey, b: 5} in a sub-chunk this shard still owns,
// while the global-max chunk lives on shard1. Since this shard owns the chunk holding the document,
// it is not a finding.
jsTest.log.info("Case 6: owned partial-MaxKey doc behind an intra-MaxKey-region split");

const ownedSplitColl = "owned_split_coll";
const ownedSplitNs = `${dbName}.${ownedSplitColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: ownedSplitNs, key: {a: 1, b: 1}}));
assert.commandWorked(
    testDB[ownedSplitColl].insert({a: MaxKey, b: 5, payload: "owned-partial-maxkey-doc"}),
);
assert.commandWorked(testDB[ownedSplitColl].insert({a: 50, b: 50, payload: "normal-doc"}));

// Split inside the leading-MaxKey region so {a: MaxKey, b: 5} and the global-max value land in
// different chunks.
assert.commandWorked(adminDB.runCommand({split: ownedSplitNs, middle: {a: MaxKey, b: 10}}));

// Move only the (empty) global-max chunk away; shard0 keeps the sub-chunk holding the document.
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: ownedSplitNs,
        bounds: [
            {a: MaxKey, b: 10},
            {a: MaxKey, b: MaxKey},
        ],
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);

// Clear the range-deletion task left by the move so it does not suppress the scan.
assert.soon(() => {
    rangeDeletionsOnPrimary(st.rs0).deleteMany({});
    st.rs0.awaitReplication();
    return rangeDeletionsOnPrimary(st.rs0).countDocuments({}) === 0;
}, "Expected range-deletion tasks on shard0 to be cleared");

assert.eq(
    1,
    st.rs0
        .getPrimary()
        .getDB(dbName)
        .getCollection(ownedSplitColl)
        .find({a: MaxKey, b: 5})
        .itcount(),
    "Expected the owned partial-MaxKey doc to remain on shard0",
);

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete with an owned partial-MaxKey doc behind an intra-MaxKey-region split",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: false, alertEmitted: false},
    "Detector must not flag a partial-MaxKey doc this shard legitimately owns",
);

resetClusterState([ownedSplitColl]);

// --- Case 7: partial-MaxKey orphan covered by an ordinary (non-global-max) deletion task --------
// {a: MaxKey, b: 5} is a transient orphan: its sub-chunk moved to shard1 and a pending
// range-deletion task whose upper bound is {a: MaxKey, b: 10} (not the global max) still covers it
// on shard0. The pending cleanup will delete it, so detection treats it as covered and does not
// flag it, even though shard0 does not own the document.
jsTest.log.info("Case 7: partial-MaxKey orphan covered by a non-global-max deletion task");

const coveredPartialColl = "covered_partial_coll";
const coveredPartialNs = `${dbName}.${coveredPartialColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: coveredPartialNs, key: {a: 1, b: 1}}));
assert.commandWorked(adminDB.runCommand({split: coveredPartialNs, middle: {a: MaxKey, b: 10}}));

// Move the global-max chunk away first so shard0 does not own it, then clear the resulting task so
// only the non-global-max task created below remains.
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: coveredPartialNs,
        bounds: [
            {a: MaxKey, b: 10},
            {a: MaxKey, b: MaxKey},
        ],
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);
assert.soon(() => {
    rangeDeletionsOnPrimary(st.rs0).deleteMany({});
    st.rs0.awaitReplication();
    return rangeDeletionsOnPrimary(st.rs0).countDocuments({}) === 0;
}, "Expected range-deletion tasks on shard0 to be cleared before creating the covering task");

// Insert into the lower sub-chunk (still on shard0), then move it away with deletion deferred so the
// orphan plus a pending task bounded by {a: MaxKey, b: 10} remain.
assert.commandWorked(
    testDB[coveredPartialColl].insert({a: MaxKey, b: 5, payload: "covered-partial-orphan-doc"}),
);
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: coveredPartialNs,
        bounds: [
            {a: MinKey, b: MinKey},
            {a: MaxKey, b: 10},
        ],
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);

const coveredPartialUUID = testDB.getCollectionInfos({name: coveredPartialColl})[0].info.uuid;
assert.soon(
    () =>
        rangeDeletionsOnPrimary(st.rs0).findOne({
            collectionUuid: coveredPartialUUID,
            "range.max.a": MaxKey,
            "range.max.b": 10,
        }) !== null,
    "Expected a pending range-deletion task bounded by {a: MaxKey, b: 10} on shard0",
);
assert.eq(
    0,
    rangeDeletionsOnPrimary(st.rs0).countDocuments({"range.max.b": MaxKey}),
    "Expected no global-max-bounded task so the covering task is the only one present",
);
assert.eq(
    1,
    st.rs0
        .getPrimary()
        .getDB(dbName)
        .getCollection(coveredPartialColl)
        .find({a: MaxKey, b: 5})
        .itcount(),
    "Expected the covered partial-MaxKey orphan to remain on shard0",
);

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete with a partial-MaxKey orphan covered by a pending range-deletion task",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: false, alertEmitted: false},
    "Detector must not flag a partial-MaxKey orphan covered by a pending range-deletion task",
);

resetClusterState([coveredPartialColl]);

// --- Case 8: orphan detected through a wider index than the shard key ---------------------------
// Shard key {a: 1} backed only by a wider compound {a: 1, b: 1} index, so detection must drop the
// trailing index field when hydrating the shard key.
jsTest.log.info(
    "Case 8: already-orphan MaxKey doc detected through a wider shard-key-prefixed index",
);

const widerIdxColl = "wider_index_coll";
const widerIdxNs = `${dbName}.${widerIdxColl}`;
// Create the compound index up front so shardCollection reuses it as the shard-key-prefixed index
// rather than creating an exact {a: 1} index.
assert.commandWorked(testDB[widerIdxColl].createIndex({a: 1, b: 1}));
assert.commandWorked(adminDB.runCommand({shardCollection: widerIdxNs, key: {a: 1}}));

// Confirm only the wider {a: 1, b: 1} index exists (no exact {a: 1} index), so the scan reads index
// entries with an extra trailing 'b' field that detection must discard to recover the {a} shard key
const widerIdxKeys = st.rs0.getPrimary().getDB(dbName).getCollection(widerIdxColl).getIndexKeys();
assert(
    widerIdxKeys.some((k) => bsonWoCompare(k, {a: 1, b: 1}) === 0),
    `Expected the compound {a: 1, b: 1} index to exist; got: ${tojson(widerIdxKeys)}`,
);
assert(
    !widerIdxKeys.some((k) => bsonWoCompare(k, {a: 1}) === 0),
    `Expected no exact {a: 1} index so the wider index is used; got: ${tojson(widerIdxKeys)}`,
);

assert.commandWorked(
    testDB[widerIdxColl].insert({a: MaxKey, b: 10, payload: "wider-index-orphan"}),
);
assert.commandWorked(testDB[widerIdxColl].insert({a: 50, b: 50, payload: "normal-doc"}));
assert.commandWorked(adminDB.runCommand({split: widerIdxNs, middle: {a: 0}}));
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: widerIdxNs,
        find: {a: 50},
        to: st.shard1.shardName,
        _waitForDelete: false,
    }),
);

const widerIdxUUID = testDB.getCollectionInfos({name: widerIdxColl})[0].info.uuid;
assert.soon(
    () =>
        rangeDeletionsOnPrimary(st.rs0).findOne({
            collectionUuid: widerIdxUUID,
            "range.max.a": MaxKey,
        }) !== null,
    "Expected the pending MaxKey-bounded range-deletion task to appear on the donor",
);

// Wipe the range-deletion task so the MaxKey doc is an uncovered orphan.
assert.commandWorked(rangeDeletionsOnPrimary(st.rs0).deleteMany({}));
st.rs0.awaitReplication();
assert.eq(
    1,
    st.rs0.getPrimary().getDB(dbName).getCollection(widerIdxColl).find({a: MaxKey}).itcount(),
    "Expected the MaxKey doc to remain on shard0 after wiping the range-deletion task",
);

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete after an orphan reachable only through a wider compound index",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: true, alertEmitted: true},
    "Detector should flag an orphan whose shard key is read through a wider compound index",
);

resetClusterState([widerIdxColl]);

// --- Case 9: empty sharded collection -----------------------------------------------------------
jsTest.log.info("Case 9: empty sharded collection must not be flagged");

const emptyColl = "empty_coll";
const emptyNs = `${dbName}.${emptyColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: emptyNs, key: {a: 1}}));

clearScanStateAndWaitMajority(st.rs0);
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Detector should complete on an empty sharded collection",
);
assertOrphanScanStats(
    st.rs0,
    {foundMaxKey: false, alertEmitted: false},
    "Detector must not flag an empty sharded collection",
);

// --- Case 10: stepdown while the scan is paused just before persisting --------------------------
// Arm the failpoint on the next primary so its scan parks just before the upsert, then stepdown to
// interrupt the parked scan (pauseWhileSet honours the opCtx interrupt from killOpForStepdown). The
// next primary must observe the doc still absent and re-run the scan to completion.
jsTest.log.info("Case 10: stepdown while the scan is paused mid-flight");

clearScanStateAndWaitMajority(st.rs0);

const nextPrimaryNode = st.rs0.getSecondary();
const hangFp = configureFailPoint(nextPrimaryNode, "hangBeforePersistingMaxKeyOrphanScanState");

st.rs0.stepUp(nextPrimaryNode);
st.rs0.waitForPrimary();
hangFp.wait({maxTimeMS: kDefaultWaitForFailPointTimeout});

assert.eq(
    st.rs0.getPrimary().host,
    nextPrimaryNode.host,
    "Expected the explicitly stepped-up node to be primary",
);

stepDownAndUp(st.rs0);

// Only nextPrimaryNode was armed, so disable via the handle.
hangFp.off();

// Clear any doc written meanwhile so the next stepup re-runs from an absent doc.
clearScanStateAndWaitMajority(st.rs0);

// Earlier cases left MaxKey orphans on shard0, so foundMaxKey may be true; only require that
// scanCompletedAt is persisted (the one-shot re-run succeeded after the interrupted sweep).
stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Expected the next primary to re-run the scan to completion after the in-flight scan was " +
        "interrupted by stepdown",
);

// --- Case 11: a non-retryable catalog read error increments maxKeyOrphanScanErrors -------------
jsTest.log.info("Case 11: non-retryable catalog read error increments maxKeyOrphanScanErrors");

const errNs = `${dbName}.err_coll`;
assert.commandWorked(adminDB.runCommand({shardCollection: errNs, key: {a: 1}}));

clearScanStateAndWaitMajority(st.rs0);

configureFailCommandAllConfigNodes(st.configRS, {
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["find"],
        namespace: "config.collections",
        errorCode: ErrorCodes.BadValue,
        failInternalCommands: true,
    },
});

stepUpAndAwaitScanState(
    st.rs0,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should still complete despite a non-retryable per-collection config read error",
);

let erroredOrphanStats;
assert.soon(
    () => {
        erroredOrphanStats = readOrphanScanStats(st.rs0);
        return erroredOrphanStats.maxKeyOrphanScanErrors >= 1;
    },
    () =>
        `maxKeyOrphanScanErrors must increment when a per-collection catalog read hits a ` +
        `non-retryable error; got ${tojson(erroredOrphanStats)}`,
);

configureFailCommandAllConfigNodes(st.configRS, {configureFailPoint: "failCommand", mode: "off"});

st.stop();
