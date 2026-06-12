/**
 * TODO (SERVER-128166): Remove this file once 9.0 becomes last LTS.
 *
 * Tests how the setMultikeyMetadata op:'c' oplog entry interacts with FCV upgrade and downgrade,
 * including stuck transitional FCV states left by a failed setFCV. The test covers the following
 * cases:
 *   - expect setMultikeyMetadata op:'c' path (with versionContext.OFCV) only at fully upgraded
 *     latestFCV.
 *   - expect legacy noop msg "Setting index to multikey" path at lastLTSFCV and at every
 *     transitional FCV (kUpgrading, kDowngrading) due to failed setFCV.
 * Does not cover:
 *   - concurrent setFCV with setMultikeyMetadata, which is not possible due to setFCV taking the
 *     multi-document transaction barrier.
 *
 * @tags: [
 *   requires_replication,
 *   uses_transactions,
 *   featureFlagReplicateMultikeynessInTransactions,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = jsTestName();
const primaryDB = primary.getDB(dbName);

function findSetMultikeyMetadataEntriesFor(node, idxName) {
    return node
        .getDB("local")
        .oplog.rs.find({op: "c", "o.setMultikeyMetadata": {$exists: true}, "o.idxName": idxName})
        .sort({$natural: 1})
        .toArray();
}

function findNoopMultikeyEntriesFor(node, idxName) {
    return node
        .getDB("local")
        .oplog.rs.find({op: "n", "o.msg": "Setting index to multikey", "o.index": idxName})
        .sort({$natural: 1})
        .toArray();
}

function runTxnInsert(collName, doc) {
    const session = primary.startSession();
    const sdb = session.getDatabase(dbName);
    session.startTransaction();
    assert.commandWorked(sdb[collName].insert(doc));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
}

function setFcv(target) {
    assert.commandWorked(
        primary.adminCommand({setFeatureCompatibilityVersion: target, confirm: true}),
    );
    rst.awaitReplication();
}

function createIndexedCollection(collName, idxName, key) {
    assert.commandWorked(primaryDB.createCollection(collName));
    assert.commandWorked(primaryDB[collName].createIndex(key, {name: idxName}));
    rst.awaitReplication();
}

function assertSetMultikeyMetadataEmitted(idxName, expectedOFCV) {
    const primaryEntries = findSetMultikeyMetadataEntriesFor(primary, idxName);
    assert.eq(
        1,
        primaryEntries.length,
        "expected exactly one setMultikeyMetadata entry on primary",
        {
            idxName,
            primaryEntries,
        },
    );
    assert.eq(
        expectedOFCV,
        primaryEntries[0].versionContext && primaryEntries[0].versionContext.OFCV,
        "wrong OFCV on primary entry",
        {idxName, entry: primaryEntries[0]},
    );

    for (const secondary of rst.getSecondaries()) {
        const secEntries = findSetMultikeyMetadataEntriesFor(secondary, idxName);
        assert.eq(1, secEntries.length, "secondary missing replicated setMultikeyMetadata entry", {
            idxName,
            host: secondary.host,
            secEntries,
        });
    }
}

function assertNoopPathTaken(idxName) {
    const setMkEntries = findSetMultikeyMetadataEntriesFor(primary, idxName);
    assert.eq(0, setMkEntries.length, "no setMultikeyMetadata expected when flag disabled by FCV", {
        idxName,
        setMkEntries,
    });

    const noopEntries = findNoopMultikeyEntriesFor(primary, idxName);
    assert.eq(
        1,
        noopEntries.length,
        "expected exactly one noop msg entry when flag disabled by FCV",
        {
            idxName,
            noopEntries,
        },
    );
}

setFcv(latestFCV);

// Phase A: fully upgraded latestFCV. Multikey-triggering write in a multi-document transaction
// emits a typed setMultikeyMetadata op:'c' entry with versionContext.OFCV == latestFCV that
// replicates to secondaries.
const idxA = "phaseA_idx";
createIndexedCollection("phaseA", idxA, {a: 1});
runTxnInsert("phaseA", {a: [1, 2, 3]});
rst.awaitReplication();
assertSetMultikeyMetadataEmitted(idxA, latestFCV);

// Phase B: setFCV fails at kDowngrading via failDowngrading failpoint (error 549181). Cluster
// stays in kDowngradingFrom_latestFCV_To_lastLTSFCV. Multikey transition must take legacy noop
// msg path because the flag is fcv_gated and enable_on_transitional_fcv_UNSAFE is unset.
const idxB = "phaseB_idx";
createIndexedCollection("phaseB", idxB, {a: 1});

const failDowngradingFp = configureFailPoint(primary, "failDowngrading");
try {
    assert.commandFailedWithCode(
        primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        549181,
    );

    runTxnInsert("phaseB", {a: [10, 20, 30]});
    assertNoopPathTaken(idxB);
} finally {
    failDowngradingFp.off();
}
setFcv(lastLTSFCV);

// Phase C: fully downgraded lastLTSFCV. Flag stays disabled; multikey transition takes legacy
// noop msg path.
const idxC = "phaseC_idx";
createIndexedCollection("phaseC", idxC, {a: 1});
runTxnInsert("phaseC", {a: [4, 5, 6]});
assertNoopPathTaken(idxC);

// Phase D: setFCV fails at kUpgrading via failUpgrading failpoint (error 549180). Cluster stays
// in kUpgradingFrom_lastLTSFCV_To_latestFCV. Same noop msg path expected.
const idxD = "phaseD_idx";
createIndexedCollection("phaseD", idxD, {a: 1});

const failUpgradingFp = configureFailPoint(primary, "failUpgrading");
try {
    assert.commandFailedWithCode(
        primary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        549180,
    );

    runTxnInsert("phaseD", {a: [100, 200, 300]});
    assertNoopPathTaken(idxD);
} finally {
    failUpgradingFp.off();
}
setFcv(latestFCV);

// Phase E: re-upgraded latestFCV. Setup matches phase A — setMultikeyMetadata emission resumes
// after the full round trip.
const idxE = "phaseE_idx";
createIndexedCollection("phaseE", idxE, {a: 1});
runTxnInsert("phaseE", {a: [7, 8, 9]});
rst.awaitReplication();
assertSetMultikeyMetadataEmitted(idxE, latestFCV);

rst.stopSet();
