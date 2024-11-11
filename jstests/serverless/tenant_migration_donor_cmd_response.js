/**
 * Verify donorStartMigration command response.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest(
    {name: jsTestName(), sharedOptions: {nodes: 1}, quickGarbageCollection: true});
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientRst = tenantMigrationTest.getRecipientRst();

const tenantId = ObjectId().str;
const tenantDB = makeTenantDB(tenantId, "DB");
let donorStateDocAfterBlocking;

function validateBlockTimestamp(cmdResponse) {
    assert.eq(timestampCmp(cmdResponse.blockTimestamp, donorStateDocAfterBlocking.blockTimestamp),
              0,
              tojson({
                  donorStartMigrationResponse: cmdResponse,
                  donorStateDoc: donorStateDocAfterBlocking,
              }));
}

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};

const hangbeforeLeavingAbortingIndexBuildstate =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState");
const hangbeforeLeavingDataSyncState =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingDataSyncState");
const hangbeforeLeavingBlockingState =
    configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

{
    jsTestLog("Verify donorStartMigration command response for state: '" +
              TenantMigrationTest.DonorState.kAbortingIndexBuilds + "'");
    hangbeforeLeavingAbortingIndexBuildstate.wait();

    const response =
        assert.commandWorked(tenantMigrationTest.runDonorStartMigration(migrationOpts));
    assert.eq(response.state, TenantMigrationTest.DonorState.kAbortingIndexBuilds, response);
    assert(!response.hasOwnProperty("abortReason"), response);
    assert(!response.hasOwnProperty("blockTimestamp"), response);

    hangbeforeLeavingAbortingIndexBuildstate.off();
}

{
    jsTestLog("Verify donorStartMigration command response for state: '" +
              TenantMigrationTest.DonorState.kDataSync + "'");
    hangbeforeLeavingDataSyncState.wait();

    const response =
        assert.commandWorked(tenantMigrationTest.runDonorStartMigration(migrationOpts));
    assert.eq(response.state, TenantMigrationTest.DonorState.kDataSync, response);
    assert(!response.hasOwnProperty("abortReason"), response);
    assert(!response.hasOwnProperty("blockTimestamp"), response);

    hangbeforeLeavingDataSyncState.off();
}

{
    jsTestLog("Verify donorStartMigration command response for state: '" +
              TenantMigrationTest.DonorState.kBlocking + "'");
    hangbeforeLeavingBlockingState.wait();

    const response =
        assert.commandWorked(tenantMigrationTest.runDonorStartMigration(migrationOpts));
    donorStateDocAfterBlocking =
        donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({_id: migrationId});
    assert.eq(response.state, TenantMigrationTest.DonorState.kBlocking, response);
    assert(!response.hasOwnProperty("abortReason"), response);
    assert(response.hasOwnProperty("blockTimestamp"), response);
    validateBlockTimestamp(response);

    hangbeforeLeavingBlockingState.off();
}

{
    jsTestLog("Verify donorStartMigration command response for state: '" +
              TenantMigrationTest.DonorState.kCommitted + "'");
    const response = assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */, true /* forgetMigration */));
    assert.eq(response.state, TenantMigrationTest.DonorState.kCommitted, response);
    assert(!response.hasOwnProperty("abortReason"), response);
    assert(response.hasOwnProperty("blockTimestamp"), response);
    validateBlockTimestamp(response);

    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, tenantId);
}

{
    jsTestLog("Verify donorStartMigration command response for state: '" +
              TenantMigrationTest.DonorState.kAborted + "'");
    const abortTenantMigration =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");

    const response = assert.commandWorked(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: true}));
    assert.eq(response.state, TenantMigrationTest.DonorState.kAborted, response);
    assert(!response.hasOwnProperty("blockTimestamp"), response);
    assert(response.hasOwnProperty("abortReason"), response);
    assert.eq(response.abortReason.code, ErrorCodes.InternalError, tojson(response));

    abortTenantMigration.off();
}
tenantMigrationTest.stop();
