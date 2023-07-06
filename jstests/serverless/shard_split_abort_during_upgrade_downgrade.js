/*
 * Prove that shard splits are aborted during FCV upgrade/downgrade.
 *
 * @tags: [requires_fcv_63, serverless]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

// Shard split commands are gated by a feature flag, which will not be supported when we
// downgrade versions. Eventually, we will run this test when we have two consecutive versions
// that support `commitShardSplit` without a feature flag. This check will be removed as part
// of SERVER-66965.
if (MongoRunner.compareBinVersions(latestFCV, "7.0") < 0) {
    quit();
}

// Skip db hash check because secondary is left with a different config.
TestData.skipCheckDBHashes = true;

const test = new ShardSplitTest({quickGarbageCollection: true});
test.addRecipientNodes();

const donorPrimary = test.donor.getPrimary();
const tenantIds = [ObjectId(), ObjectId()];

jsTestLog("Assert shard splits are aborted when downgrading.");
const downgradeFCV = lastContinuousFCV;
const hangWhileDowngradingFp = configureFailPoint(donorPrimary, "hangWhileDowngrading");
const downgradeThread = new Thread((host, downgradeFCV) => {
    const db = new Mongo(host);
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));
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
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}, donorPrimary.host);

upgradeThread.start();
hangWhileUpgradingFp.wait();
const secondSplit = test.createSplitOperation(tenantIds);
assert.commandFailedWithCode(secondSplit.commit(), ErrorCodes.TenantMigrationAborted);
hangWhileUpgradingFp.off();
upgradeThread.join();
secondSplit.forget();

test.stop();
