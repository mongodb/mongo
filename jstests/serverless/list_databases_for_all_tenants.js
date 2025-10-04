/**
 * Tests that the listDatabasesForAllTenants command is disabled.
 * This command is only available when multitenancySupport is enabled, but this feature is
 * deprecated. We can remove this test once the code for this command has been deleted
 * (see: SERVER-111128).
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kVTSKey = "secret";

function runTestNoMultiTenancySupport() {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            auth: "",
            setParameter: {
                /* This param is deprecated and should be removed (see: SERVER-111128). */
                multitenancySupport: false,
                featureFlagSecurityToken: true,
                testOnlyValidatedTenancyScopeKey: kVTSKey,
            },
        },
    });
    rst.startSet({keyFile: "jstests/libs/key1"});
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB("admin");

    assert.commandWorked(adminDB.runCommand({createUser: "internalUsr", pwd: "pwd", roles: ["__system"]}));
    assert(adminDB.auth("internalUsr", "pwd"));

    assert.commandFailedWithCode(adminDB.runCommand({listDatabasesForAllTenants: 1}), ErrorCodes.CommandNotSupported);

    rst.stopSet();
}

runTestNoMultiTenancySupport();
