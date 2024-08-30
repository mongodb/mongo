// Test SCRAM iterationCount control.

// Get a user document for username in db.
function getUserDoc(db, username) {
    const result = assert.commandWorked(
        db.runCommand({usersInfo: {user: username, db: db.getName()}, showCredentials: true}));
    assert.gt(result.users.length, 0, "No users returned: " + tojson(result));
    return result.users[0];
}

const conn = MongoRunner.runMongod({auth: ''});
const adminDB = conn.getDB('admin');

const kTestUser = 'user1';
const kTestPassword = 'pass';

adminDB.createUser({user: kTestUser, pwd: kTestPassword, roles: jsTest.adminUserRoles});
assert(adminDB.auth({user: kTestUser, pwd: kTestPassword}));

let userDoc = getUserDoc(adminDB, kTestUser);
assert.eq(10000, userDoc.credentials['SCRAM-SHA-1'].iterationCount);

// Changing iterationCount should not affect existing users.
assert.commandWorked(adminDB.runCommand({setParameter: 1, scramIterationCount: 5000}));
userDoc = getUserDoc(adminDB, kTestUser);
assert.eq(10000, userDoc.credentials['SCRAM-SHA-1'].iterationCount);

// But it should take effect when the user's password is changed.
adminDB.updateUser(kTestUser, {pwd: kTestPassword, roles: jsTest.adminUserRoles});
userDoc = getUserDoc(adminDB, kTestUser);
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
