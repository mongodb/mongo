/**
 *
 * Tests that the ShardSplit feature flag is enabled only in the proper FCV
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_util.js");

const kDummyConnStr = "mongodb://localhost/?replicaSet=foo";
function makeShardSplitTest() {
    return function(downgradeFCV) {
        function commitShardSplitCmd() {
            return {
                commitShardSplit: 1,
                tenantIds: ["foo"],
                migrationId: UUID(),
                recipientConnectionString: kDummyConnStr
            };
        }
        function abortShardSplitCmd() {
            return {abortShardSplit: 1, migrationId: UUID()};
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

        let res = adminDB.runCommand(commitShardSplitCmd());
        assert.neq(res.code,
                   6057900,
                   `commitShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);
        res = adminDB.runCommand(abortShardSplitCmd());
        assert.neq(res.code,
                   6057902,
                   `abortShardSplitCmd shouldn't reject when featureFlagShardSplit is enabled`);

        assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

        assert.commandFailedWithCode(
            adminDB.runCommand(commitShardSplitCmd()),
            6057900,
            `commitShardSplitCmd should reject when featureFlagShardSplit is disabled`);
        assert.commandFailedWithCode(
            adminDB.runCommand(abortShardSplitCmd()),
            6057902,
            `abortShardSplitCmd should reject when featureFlagShardSplit is disabled`);

        // shut down replica set
        rst.stopSet();
    };
}

runFeatureFlagMultiversionTest('featureFlagShardSplit', makeShardSplitTest());
})();
