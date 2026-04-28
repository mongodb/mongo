/**
 * Verifies that a collMod issued by the FCV upgrade path does not leak its
 * CurOp namespace onto the parent setFCV CurOp. Without the nested-CurOp
 * shield, each sub-op's AutoStatsTracker mutates the parent CurOp's NSS via
 * CurOp::enter(); with the shield, the parent's NSS stays at "admin.$cmd".
 * Regression test for SERVER-122438.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB("admin");

// Downgrade so the subsequent upgrade has real work to do (the
// _cleanUpIndexCatalogMetadataOnUpgrade step iterates every collection via collMod).
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

// Pause setFCV right after _upgradeServerMetadata completes.
const fp = configureFailPoint(primary, "hangWhileUpgrading");

const awaitSetFCV = startParallelShell(
    funWithArgs(function (fcv) {
        assert.commandWorked(db.getSiblingDB("admin").runCommand({setFeatureCompatibilityVersion: fcv, confirm: true}));
    }, latestFCV),
    primary.port,
);

try {
    fp.wait();

    const ops = adminDB
        .aggregate([
            {$currentOp: {allUsers: true, idleConnections: true}},
            {$match: {"command.setFeatureCompatibilityVersion": {$exists: true}}},
        ])
        .toArray();
    assert.eq(1, ops.length, `expected exactly one setFCV op in progress: ${tojson(ops)}`);

    assert.eq("admin.$cmd", ops[0].ns, "setFCV CurOp namespace leaked from a collMod sub-operation: " + tojson(ops[0]));
} finally {
    fp.off();
    awaitSetFCV();
}

rst.stopSet();
