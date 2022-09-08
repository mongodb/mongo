/*
 * Prove that shard splits are aborted during FCV upgrade/downgrade.
 *
 * @tags: [requires_fcv_52, featureFlagShardSplit, serverless]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

// Shard split commands are gated by a feature flag, which will not be supported when we
// downgrade versions. Eventually, we will run this test when we have two consecutive versions
// that support `commitShardSplit` without a feature flag. This check will be removed as part
// of SERVER-66965.
if (MongoRunner.compareBinVersions(latestFCV, "6.3") < 0) {
    return;
}

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;
const test = new BasicServerlessTest({
    recipientTagName: "recipientNode",
    recipientSetName: "recipient",
    quickGarbageCollection: true
});

test.addRecipientNodes();

const donorPrimary = testFixture.donor.getPrimary();
const tenantIds = ["tenant1", "tenant2"];

jsTestLog("Assert shard splits are aborted when downgrading.");
const downgradeFCV = lastContinuousFCV;
const hangWhileDowngradingFp = configureFailPoint(donorPrimary, "hangWhileDowngrading");
const downgradeThread = new Thread((host, downgradeFCV) => {
    const db = new Mongo(host);
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));
}, donorPrimary.host, downgradeFCV);

downgradeThread.start();
hangWhileDowngradingFp.wait();
const firstSplit = test.createSplitOperation(tenantIds);
assert.commandFailedWithCode(firstSplit.commit(), ErrorCodes.TenantMigrationAborted);
hangWhileDowngradingFp.off();
downgradeThread.join();
firstSplit.forget();

jsTestLog("Assert shard splits are aborted when upgrading.");
const hangWhileUpgradingFp = configureFailPoint(donorPrimary, "hangWhileUpgrading");
const upgradeThread = new Thread((host) => {
    const db = new Mongo(host);
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
}, donorPrimary.host);

upgradeThread.start();
hangWhileUpgradingFp.wait();
const secondSplit = test.createSplitOperation(tenantIds);
assert.commandFailedWithCode(secondSplit.commit(), ErrorCodes.TenantMigrationAborted);
hangWhileUpgradingFp.off();
upgradeThread.join();
secondSplit.forget();

test.stop();
})();
