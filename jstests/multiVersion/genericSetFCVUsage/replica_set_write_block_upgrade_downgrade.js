/**
 * Tests the serialization between setFeatureCompatibilityVersion and the replica set write block:
 *   - Upgrading or downgrading the FCV fails with ReplicaSetWritesBlocked (both the dry run and the
 *     real command) while replica set write blocking is enabled, and succeeds once it is disabled.
 *   - Enabling the replica set write block while an FCV transition is in progress is rejected with
 *     ConflictingOperationInProgress, because the block command serializes with setFCV on the FCV
 *     lock (a FixedFCVRegion, the same mechanism that serializes addShard with setFCV). The paused
 *     transition then completes normally.
 *
 * @tags: [
 *   requires_persistence,
 *   featureFlagBlockReplicaSetWrites,
 * ]
 */

import "jstests/multiVersion/libs/verify_versions.js";

import {
    disableReplicaSetWriteBlock,
    enableReplicaSetWriteBlock,
} from "jstests/libs/block_replica_set_writes_utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const downgradeFCV = binVersionToFCV("last-lts");
const reason = "InsufficientDiskSpace";

function runDowngrade(setFcvAdminDB) {
    return setFcvAdminDB.runCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true});
}

function runUpgrade(setFcvAdminDB) {
    return setFcvAdminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true});
}

// Both directions reject setFCV with a message mentioning replica set write blocking.
function assertSetFcvBlocked(fcvResult) {
    assert.commandFailedWithCode(fcvResult, ErrorCodes.ReplicaSetWritesBlocked);
    assert(
        fcvResult.errmsg.includes("replica set writes are blocked"),
        "Error message should mention the replica set write block",
        {fcvResult},
    );
}

function assertEnableBlockRejected(adminDB) {
    assert.commandFailedWithCode(
        adminDB.runCommand({
            blockReplicaSetWrites: 1,
            enabled: true,
            allowDeletions: false,
            reason,
        }),
        ErrorCodes.ConflictingOperationInProgress,
    );
}

function assertDowngradeBlocked(setFcvAdminDB) {
    // The dry run and the real command are both rejected without changing the FCV.
    assertSetFcvBlocked(
        setFcvAdminDB.runCommand({
            setFeatureCompatibilityVersion: downgradeFCV,
            dryRun: true,
            confirm: true,
        }),
    );
    assertSetFcvBlocked(runDowngrade(setFcvAdminDB));
}

// blockReplicaSetWrites is only available once the FCV is fully upgraded to the version that
// introduced it. Both enabling and disabling it are rejected with IllegalOperation on an older FCV.
function assertReplicaSetWriteBlockRejectedOnOldFCV(blockAdminDB) {
    assert.commandFailedWithCode(
        blockAdminDB.runCommand({
            blockReplicaSetWrites: 1,
            enabled: true,
            allowDeletions: false,
            reason,
        }),
        ErrorCodes.IllegalOperation,
    );
    assert.commandFailedWithCode(
        blockAdminDB.runCommand({blockReplicaSetWrites: 1, enabled: false, reason}),
        ErrorCodes.IllegalOperation,
    );
}

// Enable replica set write blocking and test that a new FCV downgrade is blocked. Test that retrying
// the blocked downgrade is idempotent, and that the downgrade succeeds once write blocking is disabled.
function testDowngradeBlockedWhileReplicaSetWriteBlockEnabled(blockAdminDB, setFcvAdminDB) {
    // Verify we start on the latest FCV.
    checkFCV(setFcvAdminDB, latestFCV);

    // Enable replica set write blocking and check that an FCV downgrade fails.
    enableReplicaSetWriteBlock(blockAdminDB, false /* allowDeletions */, reason);
    assertDowngradeBlocked(setFcvAdminDB);
    checkFCV(setFcvAdminDB, latestFCV);

    // Retrying the blocked downgrade is idempotent.
    assertDowngradeBlocked(setFcvAdminDB);
    checkFCV(setFcvAdminDB, latestFCV);

    // Disable replica set write blocking and check that the downgrade now succeeds.
    disableReplicaSetWriteBlock(blockAdminDB, reason);
    assert.commandWorked(runDowngrade(setFcvAdminDB));
    checkFCV(setFcvAdminDB, downgradeFCV);

    // Restore the latest FCV so subsequent transitions start from a known state, and confirm the
    // guard is re-armed after a full downgrade and upgrade cycle.
    assert.commandWorked(runUpgrade(setFcvAdminDB));
    checkFCV(setFcvAdminDB, latestFCV);
    enableReplicaSetWriteBlock(blockAdminDB, false /* allowDeletions */, reason);
    assertDowngradeBlocked(setFcvAdminDB);
    checkFCV(setFcvAdminDB, latestFCV);
    disableReplicaSetWriteBlock(blockAdminDB, reason);
}

// TODO(SERVER-123007): once 9.0 is lastLTS, remove this test and add upgrade coverage.
// The blockReplicaSetWrites command is only available once the FCV is fully upgraded to the version
// that introduced it. Downgrade to lastLTS and verify both enabling and disabling the block are
// rejected with IllegalOperation, then confirm the command works again once the FCV is upgraded back.
function testReplicaSetWriteBlockRejectedOnOldFCV(blockAdminDB, setFcvAdminDB) {
    checkFCV(setFcvAdminDB, latestFCV);
    assert.commandWorked(runDowngrade(setFcvAdminDB));
    checkFCV(setFcvAdminDB, downgradeFCV);

    assertReplicaSetWriteBlockRejectedOnOldFCV(blockAdminDB);

    // Once the FCV is upgraded back, the command is available again.
    assert.commandWorked(runUpgrade(setFcvAdminDB));
    checkFCV(setFcvAdminDB, latestFCV);
    enableReplicaSetWriteBlock(blockAdminDB, false /* allowDeletions */, reason);
    disableReplicaSetWriteBlock(blockAdminDB, reason);
}

// Pause a downgrade of a replica set in the 'downgrading' state, then try to enable write blocking. The block command
// serializes with setFCV on the FCV lock, so it is rejected with ConflictingOperationInProgress.
// Letting the downgrade proceed completes it normally.
function testBlockEnableRejectedMidDowngradeReplicaSet(primary) {
    const adminDB = primary.getDB("admin");
    checkFCV(adminDB, latestFCV);

    const fp = configureFailPoint(primary, "hangWhileDowngrading");
    const awaitDowngrade = startParallelShell(
        funWithArgs((fcv) => {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}),
            );
        }, downgradeFCV),
        primary.port,
    );

    // Wait until the downgrade is paused in the 'downgrading' state.
    fp.wait();
    checkFCV(adminDB, downgradeFCV, downgradeFCV);

    // The block cannot be enabled while the downgrade is in progress.
    assertEnableBlockRejected(adminDB);

    // Letting the downgrade proceed completes it normally.
    fp.off();
    awaitDowngrade();
    checkFCV(adminDB, downgradeFCV);

    // Restore the latest FCV for a known end state.
    assert.commandWorked(runUpgrade(adminDB));
    checkFCV(adminDB, latestFCV);

    // Verify the block can be enabled and disabled now that the downgrade completed.
    enableReplicaSetWriteBlock(adminDB, false /* allowDeletions */, reason);
    disableReplicaSetWriteBlock(adminDB, reason);
}

// Pause an upgrade of a replica set in the 'upgrading' state, then try to enable write blocking. The block command is
// rejected with ConflictingOperationInProgress. Letting the upgrade proceed completes it normally.
function testBlockEnableRejectedMidUpgradeReplicaSet(primary) {
    const adminDB = primary.getDB("admin");

    // Downgrade first.
    assert.commandWorked(runDowngrade(adminDB));
    checkFCV(adminDB, downgradeFCV);

    // Pause after the FCV document is written to kUpgrading, before the upgrade completes.
    const fp = configureFailPoint(primary, "hangAfterConfigServerChangedFCV");
    const awaitUpgrade = startParallelShell(
        funWithArgs((fcv) => {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}),
            );
        }, latestFCV),
        primary.port,
    );

    // Wait until the FCV document is kUpgrading.
    fp.wait();
    checkFCV(adminDB, downgradeFCV, latestFCV);

    // The block cannot be enabled while the upgrade is in progress.
    assertEnableBlockRejected(adminDB);

    // Letting the upgrade proceed completes it normally.
    fp.off();
    awaitUpgrade();
    checkFCV(adminDB, latestFCV);

    // Verify the block can be enabled and disabled now that the upgrade completed.
    enableReplicaSetWriteBlock(adminDB, false /* allowDeletions */, reason);
    disableReplicaSetWriteBlock(adminDB, reason);
}

// Pause a downgrade of a shard in the 'downgrading' state, then try to enable write blocking. The block command
// serializes with setFCV on the FCV lock, so it is rejected with ConflictingOperationInProgress.
// Letting the downgrade proceed completes it normally.
function testBlockEnableRejectedMidDowngradeSharded(st) {
    const shardPrimary = st.rs0.getPrimary();
    const shardAdminDB = shardPrimary.getDB("admin");
    const mongosAdminDB = st.s.getDB("admin");
    checkFCV(mongosAdminDB, latestFCV);

    const fp = configureFailPoint(shardPrimary, "hangWhileDowngrading");
    const awaitDowngrade = startParallelShell(
        funWithArgs((fcv) => {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}),
            );
        }, downgradeFCV),
        st.s.port,
    );

    // Wait until the shard is paused in the 'downgrading' state during kPrepare.
    fp.wait();
    checkFCV(mongosAdminDB, downgradeFCV, downgradeFCV);

    // The block cannot be enabled on the shard while its downgrade is in progress.
    assertEnableBlockRejected(shardAdminDB);

    // Letting the downgrade proceed completes it normally.
    fp.off();
    awaitDowngrade();
    checkFCV(mongosAdminDB, downgradeFCV);

    // Restore the latest FCV for a known end state.
    assert.commandWorked(runUpgrade(mongosAdminDB));
    checkFCV(mongosAdminDB, latestFCV);
}

// Sharded variant of testBlockEnableRejectedMidUpgradeReplicaSet: the shard is paused mid-upgrade
// and enabling the block on that shard is rejected.
function testBlockEnableRejectedMidUpgradeSharded(st) {
    const shardPrimary = st.rs0.getPrimary();
    const shardAdminDB = shardPrimary.getDB("admin");
    const mongosAdminDB = st.s.getDB("admin");

    // Downgrade first.
    assert.commandWorked(runDowngrade(mongosAdminDB));
    checkFCV(mongosAdminDB, downgradeFCV);

    // Pause the shard after it writes kUpgrading during its forwarded kStart phase.
    const fp = configureFailPoint(shardPrimary, "hangAfterConfigServerChangedFCV");
    const awaitUpgrade = startParallelShell(
        funWithArgs((fcv) => {
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}),
            );
        }, latestFCV),
        st.s.port,
    );

    // Wait until the shard has written kUpgrading.
    fp.wait();
    checkFCV(mongosAdminDB, downgradeFCV, latestFCV);

    // The block cannot be enabled on the shard while its upgrade is in progress.
    assertEnableBlockRejected(shardAdminDB);

    // Letting the upgrade proceed completes it normally.
    fp.off();
    awaitUpgrade();
    checkFCV(mongosAdminDB, latestFCV);
}

{
    jsTest.log.info("Testing setFCV / replica set write block serialization on a replica set");
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const adminDB = rst.getPrimary().getDB("admin");

    testDowngradeBlockedWhileReplicaSetWriteBlockEnabled(
        adminDB /* blockAdminDB */,
        adminDB /* setFcvAdminDB */,
    );
    testReplicaSetWriteBlockRejectedOnOldFCV(
        adminDB /* blockAdminDB */,
        adminDB /* setFcvAdminDB */,
    );
    testBlockEnableRejectedMidDowngradeReplicaSet(rst.getPrimary());
    testBlockEnableRejectedMidUpgradeReplicaSet(rst.getPrimary());
    rst.stopSet();
}

{
    jsTest.log.info("Testing setFCV / replica set write block serialization on a sharded cluster");
    const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 2}});

    testDowngradeBlockedWhileReplicaSetWriteBlockEnabled(
        st.rs0.getPrimary().getDB("admin") /* blockAdminDB */,
        st.s.getDB("admin") /* setFcvAdminDB */,
    );
    testReplicaSetWriteBlockRejectedOnOldFCV(
        st.rs0.getPrimary().getDB("admin") /* blockAdminDB */,
        st.s.getDB("admin") /* setFcvAdminDB */,
    );
    testBlockEnableRejectedMidDowngradeSharded(st);
    testBlockEnableRejectedMidUpgradeSharded(st);
    st.stop();
}
