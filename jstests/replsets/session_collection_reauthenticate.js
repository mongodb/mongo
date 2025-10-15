/**
 * This test checks that consecutive authentication attempt on the same
 * ScopedDbCollection instance does not result in reauthentication warning on primary
 *
 * Previously we had a warning so this test will fail with any earlier version of the code
 *
 *  @tags: [
 *     requires_fcv_83
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {validateSessionsCollection} from "jstests/libs/sessions_collection.js";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

const replTest = new ReplSetTest({
    name: "refresh",
    nodes: [
        {
            /* primary */
        },
        {/* secondary */ rsConfig: {priority: 0}},
    ],
    keyFile: "jstests/libs/key1",
    nodeOptions: {
        setParameter: "connPoolMaxConnsPerHost=1",
    }, // global connection pool will contain only one connection
});
const nodes = replTest.startSet();

replTest.initiate();
const kAdminUser = {
    name: "admin",
    pwd: "admin",
};

const primary = replTest.getPrimary();
// Create the admin user
const adminDB = primary.getDB("admin");
assert.commandWorked(adminDB.runCommand({createUser: kAdminUser.name, pwd: kAdminUser.pwd, roles: ["root"]}));
assert.eq(1, adminDB.auth(kAdminUser.name, kAdminUser.pwd));

const primaryAdmin = primary.getDB("admin");

replTest.awaitReplication();
const secondary = replTest.getSecondary();
const secondaryAdmin = secondary.getDB("admin");
assert.eq(1, secondaryAdmin.auth(kAdminUser.name, kAdminUser.pwd));

// Get the current value of the TTL index so that we can verify it's being properly applied.
let res = assert.commandWorked(primary.adminCommand({getParameter: 1, localLogicalSessionTimeoutMinutes: 1}));

let timeoutMinutes = res.localLogicalSessionTimeoutMinutes;

{
    // Sessions collection doesn't yet exist on primary
    validateSessionsCollection(primary, false, false, timeoutMinutes);

    replTest.awaitReplication();
    // Sessions collection doesn't yet exist on secondary
    validateSessionsCollection(secondary, false, false, timeoutMinutes);

    assert.commandWorked(primaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
    // Created sessions collection
    validateSessionsCollection(primary, true, true, timeoutMinutes);
}

{
    replTest.awaitReplication();
    // Refresh cache on secondary will authenticate a user with new ScopedDbCollection instance
    assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
    // Another refresh on secondary will authenticate again on the same instance because we limited
    // connection pool to be of size 1
    assert.commandWorked(secondaryAdmin.runCommand({refreshLogicalSessionCacheNow: 1}));
    replTest.awaitReplication();
}
// Make sure that the fix worked and no warning logged on primary and secondary
assert(
    checkLog.checkContainsWithCountJson(primary, 5626700, {}, 0, null, true),
    "Expecting not to find reauthentication warning in primary logs",
);
assert(
    checkLog.checkContainsWithCountJson(secondary, 5626700, {}, 0, null, true),
    "Expecting not to find reauthentication warning in primary logs",
);

replTest.stopSet();
