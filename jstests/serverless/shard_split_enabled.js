/**
 *
 * Tests that the ShardSplit feature flag is enabled only in the proper FCV
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");

class ShardSplitEnabledTest extends BasicServerlessTest {
    makeCommitShardSplitCmd(uuid) {
        return {
            commitShardSplit: 1,
            tenantIds: ["foo"],
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
        const test = new ShardSplitEnabledTest(
            {recipientTagName: "recipientNode", recipientSetName: "recipient"});
        test.addRecipientNodes();

        const donorPrimary = test.donor.getPrimary();
        const adminDB = donorPrimary.getDB("admin");

        // TODO(SERVER-62346): remove this when we actually split recipients
        configureFailPoint(adminDB, "skipShardSplitWaitForSplitAcceptance");

        assert(TenantMigrationUtil.isShardSplitEnabled(adminDB));
        assert.eq(getFCVConstants().latest,
                  adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

        let commitUUID = UUID();
        let res = adminDB.runCommand(test.makeCommitShardSplitCmd(commitUUID));
        assert.neq(res.code,
                   6057900,
                   `commitShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);

        res = adminDB.runCommand(test.makeForgetShardSplitCmd(commitUUID));
        assert.neq(res.code,
                   6057900,
                   `forgetShardSplit shouldn't reject when featureFlagShardSplit is enabled`);

        let abortUUID = UUID();
        res = adminDB.runCommand(test.makeAbortShardSplitCmd(abortUUID));
        assert.neq(res.code,
                   6057902,
                   `abortShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);
        res = adminDB.runCommand(test.makeForgetShardSplitCmd(abortUUID));

        assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeCommitShardSplitCmd(UUID())),
            6057900,
            `commitShardSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeAbortShardSplitCmd(UUID())),
            6057902,
            `abortShardSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeForgetShardSplitCmd(UUID())),
            6236600,
            `forgetShardSplit should reject when featureFlagShardSplit is disabled`);

        // shut down recipient nodes
        test.removeAndStopRecipientNodes();
        // shut down replica set
        test.stop();
    };
}

runFeatureFlagMultiversionTest('featureFlagShardSplit', makeShardSplitTest());
})();
