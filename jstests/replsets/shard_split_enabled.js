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
    makeCommitShardSplitCmd() {
        return {
            commitShardSplit: 1,
            tenantIds: ["foo"],
            migrationId: UUID(),
            recipientTagName: this.recipientTagName,
            recipientSetName: this.recipientSetName
        };
    }

    makeAbortShardSplitCmd() {
        return {abortShardSplit: 1, migrationId: UUID()};
    }
}

function makeShardSplitTest() {
    return function(downgradeFCV) {
        const test = new ShardSplitEnabledTest(
            {recipientTagName: "recipientNode", recipientSetName: "recipient"});
        test.addRecipientNodes();

        const donorPrimary = test.donor.getPrimary();
        const adminDB = donorPrimary.getDB("admin");

        // TODO(SERVER-63091): remove this when we actually split recipients
        configureFailPoint(adminDB, "skipShardSplitWaitForSplitAcceptance");

        assert(TenantMigrationUtil.isShardSplitEnabled(adminDB));
        assert.eq(getFCVConstants().latest,
                  adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

        let res = adminDB.runCommand(test.makeCommitShardSplitCmd());
        assert.neq(res.code,
                   6057900,
                   `commitShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);
        res = adminDB.runCommand(test.makeAbortShardSplitCmd());
        assert.neq(res.code,
                   6057902,
                   `abortShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);

        assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeCommitShardSplitCmd()),
            6057900,
            `commitShardSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(test.makeAbortShardSplitCmd()),
            6057902,
            `abortShardSplitCmd should reject when featureFlagShardSplit is disabled`);

        // shut down replica sets
        test.stop();
    };
}

runFeatureFlagMultiversionTest('featureFlagShardSplit', makeShardSplitTest());
})();
