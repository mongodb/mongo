// Ensure that created/updated users have the correct credentials.
// @tags: [requires_persistence]

(function() {
'use strict';

let mongod = MongoRunner.runMongod(
    {auth: "", setParameter: "authenticationMechanisms=SCRAM-SHA-1,SCRAM-SHA-256,PLAIN"});
const admin = mongod.getDB('admin');
const test = mongod.getDB('test');

function checkUser(userid, passwd, haveSCRAMSHA1, haveSCRAMSHA256) {
    function checkCredentialRecord(creds, hashLen, saltLen, itCount) {
        assert.eq(creds.iterationCount, itCount);
        assert.eq(creds.salt.length, saltLen);
        assert.eq(creds.storedKey.length, hashLen);
        assert.eq(creds.serverKey.length, hashLen);
    }
    function checkLogin(mech, digestOK, nodigestOK) {
        assert(test.auth({user: userid, pwd: passwd, mechanism: mech}));
        test.logout();
        assert.eq(digestOK,
                  test.auth({user: userid, pwd: passwd, mechanism: mech, digestPassword: true}));
        if (digestOK) {
            test.logout();
        }
        assert.eq(nodigestOK,
                  test.auth({user: userid, pwd: passwd, mechanism: mech, digestPassword: false}));
        if (nodigestOK) {
            test.logout();
        }
    }

    assert(admin.auth('admin', 'pass'));
    const user = admin.system.users.findOne({_id: ('test.' + userid)});
    assert.eq(user.credentials.hasOwnProperty('SCRAM-SHA-1'), haveSCRAMSHA1);
    assert.eq(user.credentials.hasOwnProperty('SCRAM-SHA-256'), haveSCRAMSHA256);

    // usersInfo contains correct mechanisms for the user
    const userInfo = assert.commandWorked(test.runCommand({usersInfo: userid}));
    assert(Array.isArray(userInfo.users[0].mechanisms));
    assert.eq(userInfo.users[0].mechanisms.includes('SCRAM-SHA-1'), haveSCRAMSHA1);
    assert.eq(userInfo.users[0].mechanisms.includes('SCRAM-SHA-256'), haveSCRAMSHA256);

    // usersInfo with showCredentials shows correct mechanisms and credentials
    const userInfoWithCredentials =
        assert.commandWorked(test.runCommand({usersInfo: userid, showCredentials: true}));
    print(tojson(userInfoWithCredentials));
    assert.eq(userInfoWithCredentials.users[0].credentials.hasOwnProperty('SCRAM-SHA-1'),
              haveSCRAMSHA1);
    assert.eq(userInfoWithCredentials.users[0].credentials.hasOwnProperty('SCRAM-SHA-256'),
              haveSCRAMSHA256);
    assert(Array.isArray(userInfoWithCredentials.users[0].mechanisms));
    assert.eq(userInfoWithCredentials.users[0].mechanisms.includes('SCRAM-SHA-1'), haveSCRAMSHA1);
    assert.eq(userInfoWithCredentials.users[0].mechanisms.includes('SCRAM-SHA-256'),
              haveSCRAMSHA256);
    admin.logout();

    if (haveSCRAMSHA1) {
        checkCredentialRecord(user.credentials['SCRAM-SHA-1'], 28, 24, 10000);
        checkLogin('SCRAM-SHA-1', true, false);
        checkLogin('PLAIN', false, true);
    }
    if (haveSCRAMSHA256) {
        checkCredentialRecord(user.credentials['SCRAM-SHA-256'], 44, 40, 15000);
        checkLogin('SCRAM-SHA-256', false, true);
        checkLogin('PLAIN', false, true);
    }
}

admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
assert(admin.auth('admin', 'pass'));

function createUser(db, user) {
    assert(admin.auth('admin', 'pass'));
    db.createUser(user);
    admin.logout();
}

function createUserThrows(db, user) {
    assert(admin.auth('admin', 'pass'));
    assert.throws(() => db.createUser(user));
    admin.logout();
}

// Unknown mechanism.
createUserThrows(test, {
    user: 'shalala',
    pwd: 'pass',
    roles: jsTest.basicUserRoles,
    mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-LA-LA'],
});

// By default, users are created with both SCRAM variants.
createUser(test, {user: 'user', pwd: 'pass', roles: jsTest.basicUserRoles});
checkUser('user', 'pass', true, true);

// Request SHA1 only.
createUser(
    test,
    {user: 'sha1user', pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: ['SCRAM-SHA-1']});
checkUser('sha1user', 'pass', true, false);

// Request SHA256 only.
createUser(
    test,
    {user: 'sha256user', pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: ['SCRAM-SHA-256']});
checkUser('sha256user', 'pass', false, true);

// Fail passing an empty mechanisms field.
createUserThrows(test,
                 {user: 'userNoMech', pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: []});

// Repeat above, but request client-side digesting.
// Only the SCRAM-SHA-1 exclusive version should succeed.

createUserThrows(
    test, {user: 'user2', pwd: 'pass', roles: jsTest.basicUserRoles, passwordDisgestor: 'client'});

createUser(test, {
    user: 'sha1user2',
    pwd: 'pass',
    roles: jsTest.basicUserRoles,
    mechanisms: ['SCRAM-SHA-1'],
    passwordDigestor: 'client'
});
checkUser('sha1user2', 'pass', true, false);

createUserThrows(test, {
    user: 'sha256user2',
    pwd: 'pass',
    roles: jsTest.basicUserRoles,
    mechanisms: ['SCRAM-SHA-256'],
    passwordDigestor: 'client'
});

function updateUser(db, user, props) {
    assert(admin.auth('admin', 'pass'));
    db.updateUser(user, props);
    admin.logout();
}

function updateUserThrows(db, user, props) {
    assert(admin.auth('admin', 'pass'));
    assert.throws(() => db.updateUser(user, props));
    admin.logout();
}

// Update original 1/256 user to just sha-1.
updateUser(test, 'user', {pwd: 'pass1', mechanisms: ['SCRAM-SHA-1']});
checkUser('user', 'pass1', true, false);

// Then flip to 256-only
updateUser(test, 'user', {pwd: 'pass256', mechanisms: ['SCRAM-SHA-256']});
checkUser('user', 'pass256', false, true);

// And back to (default) all.
updateUser(test, 'user', {pwd: 'passAll'});
checkUser('user', 'passAll', true, true);

// Trim out mechanisms without changing password.
updateUser(test, 'user', {mechanisms: ['SCRAM-SHA-256']});
checkUser('user', 'passAll', false, true);

// Fail when mechanisms is not a subset of the current user.
updateUserThrows(test, 'user', {mechanisms: ['SCRAM-SHA-1']});

// Fail when passing an empty mechanisms field.
updateUserThrows(test, 'user', {pwd: 'passEmpty', mechanisms: []});

// Succeed if we're using SHA-1 only.
createUser(
    test, {user: "\u2168", pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: ['SCRAM-SHA-1']});
checkUser("\u2168", 'pass', true, false);

assert(admin.auth('admin', 'pass'));

// Demonstrate that usersInfo returns all users with mechanisms lists
const allUsersInfo = assert.commandWorked(test.runCommand({usersInfo: 1}));
allUsersInfo.users.forEach(function(userObj) {
    assert(Array.isArray(userObj.mechanisms));
});

// Demonstrate that usersInfo can return all users with credentials
const allUsersInfoWithCredentials =
    assert.commandWorked(test.runCommand({usersInfo: 1, showCredentials: true}));
allUsersInfoWithCredentials.users.forEach(function(userObj) {
    assert(userObj.credentials !== undefined);
    assert(!Array.isArray(userObj.credentials));
    assert(userObj.mechanisms !== undefined);
    assert(Array.isArray(userObj.mechanisms));
});

// Demonstrate that usersInfo can find SCRAM-SHA-1 users
const allSCRAMSHA1UsersInfo =
    assert.commandWorked(test.runCommand({usersInfo: 1, filter: {mechanisms: "SCRAM-SHA-1"}}));
let foundUsers = [];
allSCRAMSHA1UsersInfo.users.forEach(function(userObj) {
    foundUsers.push(userObj.user);
});
assert.eq(["sha1user", "sha1user2", "\u2168"], foundUsers);

// Demonstrate that usersInfo can find SCRAM-SHA-256 users
const allSCRAMSHA256UsersInfo =
    assert.commandWorked(test.runCommand({usersInfo: 1, filter: {mechanisms: "SCRAM-SHA-256"}}));
foundUsers = [];
allSCRAMSHA256UsersInfo.users.forEach(function(userObj) {
    foundUsers.push(userObj.user);
});
assert.eq(["sha256user", "user"], foundUsers);

MongoRunner.stopMongod(mongod);

// Ensure mechanisms can be enabled and disabled.
function restartWithOneMech(mech) {
    const sha1ok = (mech === 'SCRAM-SHA-1');
    const sha256ok = (mech === 'SCRAM-SHA-256');
    mongod = MongoRunner.runMongod({
        auth: "",
        setParameter: "authenticationMechanisms=" + mech,
        restart: mongod,
        noCleanData: true
    });

    const test = mongod.getDB('test');
    assert.eq(test.auth("sha1user", "pass"), sha1ok);
    if (sha1ok) {
        test.logout();
    }
    assert.eq(test.auth("sha256user", "pass"), sha256ok);
    if (sha256ok) {
        test.logout();
    }
    assert(mongod.getDB('admin').auth('admin', 'pass'));
    MongoRunner.stopMongod(mongod);
}

restartWithOneMech('SCRAM-SHA-1');
restartWithOneMech('SCRAM-SHA-256');
})();
