/**
 * Test that a tenant migration fails when the given TenantId option is not properly formatted.
 * A tenantId should be of OID Format.
 *
 * @tags: [requires_fcv_52, serverless]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

let adminDB = tenantMigrationTest.getDonorPrimary().getDB('admin');

const arrayOfBadTenantIds = {
    "BadTenantId": ErrorCodes.BadValue,
    "636d957b2646ddfaf9b5e" /*cropped oid*/: ErrorCodes.BadValue,
    "": ErrorCodes.BadValue,
};

Object.entries(arrayOfBadTenantIds).forEach(([tenantId, errCode]) => {
    jsTest.log(`Going to start a tenant migration with a bad tenantId "${tenantId}" and fail.`);
    let startMigrationCmd = {
        donorStartMigration: 1,
        tenantId: tenantId,
        migrationId: UUID(),
        readPreference: {mode: "primary"}
    };
    assert.commandFailedWithCode(adminDB.runCommand(startMigrationCmd), errCode);
});

tenantMigrationTest.stop();
