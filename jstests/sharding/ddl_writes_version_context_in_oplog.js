/**
 * Sharded DDL operations use a fixed FCV snapshot ("Operation FCV") over their lifetime, so that
 * their view of feature flags is isolated. This test checks that their oplog entries replicate
 * that FCV, which ensures secondaries also view the same feature flags when applying it.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
const db = st.s.getDB("test");

const initialFCV =
    db.getSiblingDB("admin").system.version.findOne({_id: 'featureCompatibilityVersion'}).version;
const shouldSnapshotOFCV = FeatureFlagUtil.isPresentAndEnabled(db, "SnapshotFCVInDDLCoordinators");

function createCollectionAndAssertVersionContext(collName, expectedVersionContext) {
    // Create a sharded collection. Since it is a sharded DDL, we expect it to use an Operation FCV.
    const ns = db.getName() + "." + collName;
    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: 1}}));
    st.awaitReplicationOnShards();

    // Check that the version context has been replicated in the create op
    const filter = {op: "c", ns: db.getName() + '.$cmd', "o.create": collName};
    for (let node of st.rs0.nodes) {
        const oplogEntry = node.getDB("local").oplog.rs.find(filter).sort({$natural: -1}).next();
        assert.docEq(expectedVersionContext, oplogEntry.versionContext, tojson(oplogEntry));
    }
}

createCollectionAndAssertVersionContext("coll",
                                        shouldSnapshotOFCV ? {OFCV: initialFCV} : undefined);

// Verify that the transitional upgrade/downgrade FCVs have corresponding Operation FCV values.
if (initialFCV == latestFCV) {  // setFCV is not possible in multiversion suites
    // Transitional downgrade FCV
    const hangWhileDowngradingFp = configureFailPoint(st.rs0.getPrimary(), "hangWhileDowngrading");
    const downgradeThread = startParallelShell(function() {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    }, st.s.port);
    hangWhileDowngradingFp.wait();
    createCollectionAndAssertVersionContext(
        "collDowngrade",
        shouldSnapshotOFCV ? {OFCV: `downgrading from ${latestFCV} to ${lastLTSFCV}`} : undefined);
    hangWhileDowngradingFp.off();
    downgradeThread();

    // Transitional upgrade FCV
    const hangWhileUpgradingFp = configureFailPoint(st.rs0.getPrimary(), "hangWhileUpgrading");
    const upgradeThread = startParallelShell(function() {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    }, st.s.port);
    hangWhileUpgradingFp.wait();
    createCollectionAndAssertVersionContext(
        "collUpgrade",
        shouldSnapshotOFCV ? {OFCV: `upgrading from ${lastLTSFCV} to ${latestFCV}`} : undefined);
    hangWhileUpgradingFp.off();
    upgradeThread();
}

st.stop();
