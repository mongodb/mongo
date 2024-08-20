/**
 * Tests that 'defaultMaxTimeMS' is applied correctly when used with tenantId.
 *
 * @tags: [
 *   requires_auth,
 *   requires_replication,
 *   # Transactions aborted upon fcv upgrade or downgrade; cluster parameters use internal txns.
 *   uses_transactions,
 *   featureFlagSecurityToken,
 *   requires_fcv_80,
 * ]
 */

import {runCommandWithSecurityToken} from "jstests/libs/multitenancy_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const keyFile = "jstests/libs/key1";
const tenantId1 = ObjectId();
const tenantId2 = ObjectId();
const vtsKey = "secret";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        auth: "",
        setParameter: {
            multitenancySupport: true,
            testOnlyValidatedTenancyScopeKey: vtsKey,
        }
    },
    keyFile
});

rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const authDbName = "admin";
const adminDB = primary.getDB(authDbName);
const adminUser = {
    userName: "adminUser",
    password: "adminPwd",
    roles: ["__system"]
};
assert.commandWorked(adminDB.runCommand({
    createUser: adminUser.userName,
    pwd: adminUser.password,
    roles: adminUser.roles,
}));

adminDB.auth(adminUser.userName, adminUser.password);
const unsignedToken1 = _createTenantToken({tenant: tenantId1});
const unsignedToken2 = _createTenantToken({tenant: tenantId2});

// Sets the defaultMaxTimeMS with tenantId1.
assert.commandWorked(runCommandWithSecurityToken(
    unsignedToken1, adminDB, {setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}));

// Prepare a regular user without the 'bypassDefaultMaxTimeMS' privilege.
adminDB.createRole({
    role: "useTenantRole",
    privileges: [
        {resource: {cluster: true}, actions: ["useTenant"]},
    ],
    roles: []
});
adminDB.createUser({user: 'regularUser', pwd: 'password', roles: ["useTenantRole"]});
const regularUserDB = new Mongo(primary.host).getDB('admin');
assert(regularUserDB.auth('regularUser', 'password'), "Auth failed");

// Running a slow command with tenantId1 will time out.
assert.commandFailedWithCode(runCommandWithSecurityToken(unsignedToken1, regularUserDB, {
                                 sleep: 1,
                                 millis: 300,
                             }),
                             ErrorCodes.MaxTimeMSExpired);

// Specifying a per-query maxTimeMS will overwrite the default value.
assert.commandWorked(runCommandWithSecurityToken(unsignedToken1, regularUserDB, {
    sleep: 1,
    millis: 300,
    maxTimeMS: 0,
}));

// The default value of tenantId1 should not affect operations run with other tenantIds.
assert.commandWorked(runCommandWithSecurityToken(unsignedToken2, regularUserDB, {
    sleep: 1,
    millis: 300,
}));

// Sets the global defaultMaxTimeMS to a small value.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 1}}}));

// Without a tenant-specific default value, operations will use the global default value, which
// causes a timeout.
assert.commandFailedWithCode(runCommandWithSecurityToken(unsignedToken2, regularUserDB, {
                                 sleep: 1,
                                 millis: 300,
                             }),
                             ErrorCodes.MaxTimeMSExpired);

// Again, the global default value can still be overwritten by a per-query value.
assert.commandWorked(runCommandWithSecurityToken(unsignedToken2, regularUserDB, {
    sleep: 1,
    millis: 300,
    maxTimeMS: 0,
}));

// Sets the default timeout of tenantId1 with a higher value.
assert.commandWorked(runCommandWithSecurityToken(
    unsignedToken1, adminDB, {setClusterParameter: {defaultMaxTimeMS: {readOperations: 60000}}}));

// With both the global and tenant-specific default timeout, the operation run by a tenant will
// choose its tenant-specific default.
assert.commandWorked(runCommandWithSecurityToken(unsignedToken1, regularUserDB, {
    sleep: 1,
    millis: 300,
}));

adminDB.logout();
regularUserDB.logout();

rst.stopSet();
