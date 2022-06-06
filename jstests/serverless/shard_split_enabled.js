/**
 *
 * Tests that the ShardSplit feature flag is enabled only in the proper FCV
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

const tenantIds = ["foo"];
class ShardSplitEnabledTest extends BasicServerlessTest {
    makeCommitShardSplitCmd(uuid) {
        return {
            commitShardSplit: 1,
            tenantIds,
            migrationId: uuid,
            recipientTagName: this.recipientTagName,
            recipientSetName: this.recipientSetName
        };
    }

    makeAbortShardSplitCmd(uuid) {
        return {abortShardSplit: 1, migrationId: uuid};
    }

    makeForgetShardSplitCmd(uuid) {
        return {forgetShardSplit: 1, migrationId: uuid};
    }
}

function makeShardSplitTest() {
    return function(downgradeFCV) {
        const test = new ShardSplitEnabledTest({
            recipientTagName: "recipientNode",
            recipientSetName: "recipient",
            quickGarbageCollection: true
        });
        test.addRecipientNodes();

        const donorPrimary = test.donor.getPrimary();
        const adminDB = donorPrimary.getDB("admin");

        assert(TenantMigrationUtil.isShardSplitEnabled(adminDB));
        assert.eq(getFCVConstants().latest,
                  adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

        let commitUUID = UUID();
        let res = adminDB.runCommand(test.makeCommitShardSplitCmd(commitUUID));
        assert.neq(res.code,
                   ErrorCodes.IllegalOperation,
                   `commitShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);

        test.removeRecipientNodesFromDonor();
        res = adminDB.runCommand(test.makeForgetShardSplitCmd(commitUUID));
        assert.neq(res.code,
                   ErrorCodes.IllegalOperation,
                   `forgetShardSplit shouldn't reject when featureFlagShardSplit is enabled`);

        test.waitForGarbageCollection(commitUUID, tenantIds);

        let abortUUID = UUID();
        res = adminDB.runCommand(test.makeAbortShardSplitCmd(abortUUID));
        assert.neq(res.code,
                   ErrorCodes.IllegalOperation,
                   `abortShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);

        assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeCommitShardSplitCmd(UUID())),
            ErrorCodes.IllegalOperation,
            `commitShardSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeAbortShardSplitCmd(UUID())),
            ErrorCodes.IllegalOperation,
            `abortShardSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeForgetShardSplitCmd(UUID())),
            ErrorCodes.IllegalOperation,
            `forgetShardSplit should reject when featureFlagShardSplit is disabled`);

        // shut down replica set
        test.stop();
    };
}

runFeatureFlagMultiversionTest('featureFlagShardSplit', makeShardSplitTest());
})();
