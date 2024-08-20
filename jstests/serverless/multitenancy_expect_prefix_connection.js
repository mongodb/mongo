// Test parsing expectPrefix out of an unsigned security token, and enforcing that once a connection
// is marked as being from atlas proxy it may never unset that property
// @tags: [featureFlagSecurityToken]

import {ReplSetTest} from "jstests/libs/replsettest.js";

const tenantID = ObjectId();
const kVTSKey = 'secret';
const kDbName = 'myDb';
const kCollName = 'myColl';

const opts = {
    auth: '',
    setParameter: {
        multitenancySupport: true,
        testOnlyValidatedTenancyScopeKey: kVTSKey,
    },
};

const rst = new ReplSetTest({nodes: 2, nodeOptions: opts});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

let conn = rst.getPrimary();
const admin = conn.getDB('admin');

// Must be authenticated as a user with read/write privileges on non-normal collections, since
// we are accessing system.users for another tenant.
assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['__system']}));
assert(admin.auth('admin', 'pwd'));
// Make a less-privileged base user.
assert.commandWorked(
    admin.runCommand({createUser: 'baseuser', pwd: 'pwd', roles: ['readWriteAnyDatabase']}));

const testDb = conn.getDB(tenantID.str + "_myDb");
let countCmd = {count: kCollName, query: {myDoc: 0}};

// Set the initial security token. This simulates atlas proxy behavior.
const setInitialSecurityTokenWithExpectPrefix = function() {
    conn._setSecurityToken(_createTenantToken({tenant: tenantID, expectPrefix: true}));
    assert.commandWorked(testDb.runCommand(countCmd));
}();

// Test this second token will throw an assert because we can't change the token once it's set.
const changeTokenExpectPrefixFalse = function() {
    conn._setSecurityToken(_createTenantToken({tenant: tenantID, expectPrefix: false}));
    assert.commandFailedWithCode(testDb.runCommand(countCmd),
                                 8154400 /*conn protocol can only change once*/);
}();

// Test this third token will throw an assert because we can't change the token once it's set.
const changeTokenMissingExpectPrefix = function() {
    conn._setSecurityToken(_createTenantToken({tenant: tenantID}));
    assert.commandFailedWithCode(testDb.runCommand(countCmd),
                                 8154400 /*conn protocol can only change once*/);
}();

// Setting the same unsigned token works because same token is the same as using the original one.
// Nothing has changed.
const changeTokenBackToOriginal = function() {
    conn._setSecurityToken(_createTenantToken({tenant: tenantID, expectPrefix: true}));
    assert.commandWorked(testDb.runCommand(countCmd));
}();

// no tenant given so we shouldn't see the doc.
conn._setSecurityToken(undefined);

rst.stopSet();
