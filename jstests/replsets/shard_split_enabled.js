/**
 *
 * Tests that the ShardSplit feature flag is enabled only in the proper FCV
 * @tags: [requires_fcv_51, featureFlagShardSplit]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_util.js");

const kDummyConnStr = "mongodb://localhost/?replicaSet=foo";
function makeShardSplitTest() {
    return function(downgradeFCV) {
        function donorStartSplitCmd() {
            return {
                donorStartSplit: 1,
                tenantId: "foo",
                migrationId: UUID(),
                recipientConnectionString: kDummyConnStr
            };
        }
        function donorForgetSplitCmd() {
            return {donorForgetSplit: 1, migrationId: UUID()};
        }
        function donorAbortSplitCmd() {
            return {donorAbortSplit: 1, migrationId: UUID()};
        }

        // start up a replica set
        // server-side setup
        const rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
        const primary = rst.getPrimary();
        const adminDB = primary.getDB("admin");

        assert(TenantMigrationUtil.isShardSplitEnabled(adminDB));
        assert.eq(getFCVConstants().latest,
                  adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

        let res = adminDB.runCommand(donorStartSplitCmd());
        assert.neq(res.code,
                   6057900,
                   `donorStartSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);
        res = adminDB.runCommand(donorForgetSplitCmd());
        assert.neq(res.code,
                   6057901,
                   `donorForgetSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);
        res = adminDB.runCommand(donorAbortSplitCmd());
        assert.neq(res.code,
                   6057902,
                   `donorAbortSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);

        assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

        assert.commandFailedWithCode(
            adminDB.runCommand(donorStartSplitCmd()),
            6057900,
            `donorStartSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(donorForgetSplitCmd()),
            6057901,
            `donorForgetSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(donorAbortSplitCmd()),
            6057902,
            `donorAbortSplitCmd should reject when featureFlagShardSplit is disabled`);

        // shut down replica set
        rst.stopSet();
    };
}

runFeatureFlagMultiversionTest('featureFlagShardSplit', makeShardSplitTest());
})();
