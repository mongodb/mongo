/**
 * Tests that the "shard merge" protocol is enabled only in the proper FCV.
 *
 * @tags: [
 *   requires_shard_merge,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function runTest(downgradeFCV) {
    const rst = new ReplSetTest({nodes: 1, serverless: true});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    const adminDB = primary.getDB("admin");
    const kDummyConnStr = "mongodb://localhost/?replicaSet=foo";
    const readPreference = {mode: 'primary'};

    // A function, not a constant, to ensure unique UUIDs.
    function donorStartMigrationCmd() {
        return {
            donorStartMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            recipientConnectionString: kDummyConnStr,
            readPreference: readPreference,
            tenantIds: [ObjectId()]
        };
    }

    function recipientSyncDataCmd() {
        return {
            recipientSyncData: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            tenantIds: [ObjectId()],
            donorConnectionString: kDummyConnStr,
            readPreference: readPreference,
            startMigrationDonorTimestamp: Timestamp(1, 1),
        };
    }

    function recipientForgetMigrationCmd() {
        return {
            recipientForgetMigration: 1,
            protocol: "shard merge",
            migrationId: UUID(),
            tenantIds: [ObjectId()],
            donorConnectionString: kDummyConnStr,
            readPreference: readPreference,
            decision: "committed"
        };
    }

    function func(cmd) {
        return eval(cmd + "Cmd()");
    }

    function testCommandWithShardMerge(cmd) {
        let msg = cmd + " shouldn't reject 'shard merge' protocol when it's enabled";
        assert.commandWorked(adminDB.runCommand(func(cmd)), msg);
    }

    function testCommandFailsWithShardMerge(cmd, reason, expectedErrorMsg, expectedErrorCode) {
        let msg = `${cmd} ${reason}`;
        let response =
            assert.commandFailedWithCode(adminDB.runCommand(func(cmd)), expectedErrorCode, msg);
        assert.neq(-1,
                   response.errmsg.indexOf(expectedErrorMsg),
                   "Error message did not contain '" + expectedErrorMsg + "', found:\n" +
                       tojson(response));
    }

    function testCommandFailsShardMergeNotSupported(cmd) {
        testCommandFailsWithShardMerge(cmd,
                                       "should reject 'shard merge' protocol when it's disabled",
                                       "protocol 'shard merge' not supported",
                                       ErrorCodes.IllegalOperation);
    }

    function testCommandFailsProtocolFieldNotSupported(cmd) {
        testCommandFailsWithShardMerge(cmd,
                                       "should reject 'protocol' field when it's disabled",
                                       "'protocol' field is not supported for FCV below 5.2'",
                                       ErrorCodes.InvalidOptions);
    }

    // Enable below fail points to prevent starting the donor/recipient POS instance.
    configureFailPoint(primary, "returnResponseCommittedForDonorStartMigrationCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientSyncDataCmd");
    configureFailPoint(primary, "returnResponseOkForRecipientForgetMigrationCmd");

    // Preconditions: the shard merge feature is enabled and our fresh RS is on the latest FCV.
    assert.eq(getFCVConstants().latest,
              adminDB.system.version.findOne({_id: 'featureCompatibilityVersion'}).version);

    // Shard merge is enabled, so this call should work.
    let cmds = ["donorStartMigration", "recipientSyncData", "recipientForgetMigration"];
    cmds.forEach((cmd) => {
        testCommandWithShardMerge(cmd);
    });

    assert.commandWorked(
        adminDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));
    // Now that FCV is downgraded, shard merge is automatically disabled.
    cmds.forEach((cmd) => {
        if (MongoRunner.compareBinVersions(downgradeFCV, "5.2") >= 0) {
            // The "protocol" field is ok, but it can't be "shard merge".
            testCommandFailsShardMergeNotSupported(cmd);
        } else {
            // The "protocol" field is not supported.
            testCommandFailsProtocolFieldNotSupported(cmd);
        }
    });

    rst.stopSet();
}

runFeatureFlagMultiversionTest('featureFlagShardMerge', runTest);
