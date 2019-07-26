// Test SCRAM iterationCount control.

(function() {
'use strict';

load('./jstests/multiVersion/libs/auth_helpers.js');

const conn = MongoRunner.runMongod({auth: ''});
const adminDB = conn.getDB('admin');

adminDB.createUser({user: 'user1', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(adminDB.auth({user: 'user1', pwd: 'pass'}));

var userDoc = getUserDoc(adminDB, 'user1');
assert.eq(10000, userDoc.credentials['SCRAM-SHA-1'].iterationCount);

// Changing iterationCount should not affect existing users.
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramIterationCount: 5000}));
userDoc = getUserDoc(adminDB, 'user1');
assert.eq(10000, userDoc.credentials['SCRAM-SHA-1'].iterationCount);

// But it should take effect when the user's password is changed.
adminDB.updateUser('user1', {pwd: 'pass', roles: jsTest.adminUserRoles});
userDoc = getUserDoc(adminDB, 'user1');
assert.eq(5000, userDoc.credentials['SCRAM-SHA-1'].iterationCount);

// Test (in)valid values for scramIterationCount. 5000 is the minimum value.
assert.commandFailed(adminDB.runCommand({setParameter: 1, scramIterationCount: 4999}));
assert.commandFailed(adminDB.runCommand({setParameter: 1, scramIterationCount: -5000}));
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramIterationCount: 5000}));
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramIterationCount: 10000}));
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramIterationCount: 1000000}));

assert.commandFailed(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: -5000}));
assert.commandFailed(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: 4095}));
assert.commandFailed(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: 4096}));
assert.commandFailed(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: 4999}));
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: 5000}));
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: 10000}));
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramSHA256IterationCount: 1000000}));

MongoRunner.stopMongod(conn);
})();
