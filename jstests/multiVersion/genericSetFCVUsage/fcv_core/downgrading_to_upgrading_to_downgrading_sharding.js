/**
 * Regression test for SERVER-125824:
 * Test that in a sharded cluster, the setFCV sharding protocol correctly handles a
 * "downgrading" -> "downgrading to upgrading" -> "upgrading to downgrading" FCV transition sequence.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1, config: 1});
const configPrimary = st.configRS.getPrimary();
const shardPrimary = st.rs0.getPrimary();

checkFCV(configPrimary.getDB("admin"), latestFCV);
checkFCV(shardPrimary.getDB("admin"), latestFCV);

// (1) Start a downgrade to lastLTS FCV, but fail in the "prepare" phase.
let fp = configureFailPoint(
    shardPrimary,
    "failDowngradeValidationDueToIncompatibleFeature",
    {},
    {skip: 1} /* Skip so we fail during "prepare", not during the dry-run */,
);
assert.commandFailedWithCode(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    ErrorCodes.CannotDowngrade,
);
fp.off();

// Both the configsvr and the shardsvr are in the "downgrading" FCV.
checkFCV(configPrimary.getDB("admin"), lastLTSFCV, lastLTSFCV);
checkFCV(shardPrimary.getDB("admin"), lastLTSFCV, lastLTSFCV);

// (2) Attempt to upgrade back to latest FCV, but fail immediately after the configsvr enters the "prepare" phase.
fp = configureFailPoint(configPrimary, "failBeforeSendingShardsToDowngradingOrUpgrading");
assert.commandFailedWithCode(
    st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    6794600,
);
fp.off();

// The configsvr is in "upgrading", but the shardsvr is still in "downgrading".
checkFCV(configPrimary.getDB("admin"), lastLTSFCV, latestFCV);
checkFCV(shardPrimary.getDB("admin"), lastLTSFCV, lastLTSFCV);

// (3) Start another downgrade to lastLTS FCV and check that it works.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);
checkFCV(configPrimary.getDB("admin"), lastLTSFCV);
checkFCV(shardPrimary.getDB("admin"), lastLTSFCV);

st.stop();
