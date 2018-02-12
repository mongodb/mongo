// Ensure that created/updated users have the correct credentials.

(function() {
    'use strict';

    const mongod = MongoRunner.runMongod(
        {auth: "", setParameter: "authenticationMechanisms=SCRAM-SHA-1,SCRAM-SHA-256,PLAIN"});
    assert(mongod);
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
            assert.eq(
                digestOK,
                test.auth({user: userid, pwd: passwd, mechanism: mech, digestPassword: true}));
            if (digestOK) {
                test.logout();
            }
            assert.eq(
                nodigestOK,
                test.auth({user: userid, pwd: passwd, mechanism: mech, digestPassword: false}));
            if (nodigestOK) {
                test.logout();
            }
        }
        const user = admin.system.users.findOne({_id: ('test.' + userid)});
        assert.eq(user.credentials.hasOwnProperty('SCRAM-SHA-1'), haveSCRAMSHA1);
        assert.eq(user.credentials.hasOwnProperty('SCRAM-SHA-256'), haveSCRAMSHA256);
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

    // Test FCV 3.6 mode first.
    assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "3.6"}));

    // By default, only create SHA-1 in 3.6 FCV mode.
    test.createUser({user: 'sha1default', pwd: 'pass', roles: jsTest.basicUserRoles});
    checkUser('sha1default', 'pass', true, false);

    // SCRAM-SHA-256 not available.
    assert.throws(function() {
        test.createUser({
            user: 'sha256failure',
            pwd: 'pass',
            roles: jsTest.basicUserRoles,
            mechanisms: ['SCRAM-SHA-256']
        });
    });

    // Unknown mechanism.
    assert.throws(function() {
        test.createUser({
            user: 'shalala',
            pwd: 'pass',
            roles: jsTest.basicUserRoles,
            mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-LA-LA'],
        });
    });

    // Now do general testing with SCRAM-SHA-256 enabled.
    assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "4.0"}));

    // By default, users are created with both SCRAM variants.
    test.createUser({user: 'user', pwd: 'pass', roles: jsTest.basicUserRoles});
    checkUser('user', 'pass', true, true);

    // Request SHA1 only.
    test.createUser(
        {user: 'sha1user', pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: ['SCRAM-SHA-1']});
    checkUser('sha1user', 'pass', true, false);

    // Request SHA256 only.
    test.createUser({
        user: 'sha256user',
        pwd: 'pass',
        roles: jsTest.basicUserRoles,
        mechanisms: ['SCRAM-SHA-256']
    });
    checkUser('sha256user', 'pass', false, true);

    // Fail passing an empty mechanisms field.
    assert.throws(function() {
        test.createUser(
            {user: 'userNoMech', pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: []});
    });

    // Repeat above, but request client-side digesting.
    // Only the SCRAM-SHA-1 exclusive version should succeed.

    assert.throws(function() {
        test.createUser({
            user: 'user2',
            pwd: 'pass',
            roles: jsTest.basicUserRoles,
            passwordDisgestor: 'client'
        });
    });

    test.createUser({
        user: 'sha1user2',
        pwd: 'pass',
        roles: jsTest.basicUserRoles,
        mechanisms: ['SCRAM-SHA-1'],
        passwordDigestor: 'client'
    });
    checkUser('sha1user2', 'pass', true, false);

    assert.throws(function() {
        test.createUser({
            user: 'sha256user2',
            pwd: 'pass',
            roles: jsTest.basicUserRoles,
            mechanisms: ['SCRAM-SHA-256'],
            passwordDigestor: 'client'
        });
    });

    // Update original 1/256 user to just sha-1.
    test.updateUser('user', {pwd: 'pass1', mechanisms: ['SCRAM-SHA-1']});
    checkUser('user', 'pass1', true, false);

    // Then flip to 256-only
    test.updateUser('user', {pwd: 'pass256', mechanisms: ['SCRAM-SHA-256']});
    checkUser('user', 'pass256', false, true);

    // And back to (default) all.
    test.updateUser('user', {pwd: 'passAll'});
    checkUser('user', 'passAll', true, true);

    // Trim out mechanisms without changing password.
    test.updateUser('user', {mechanisms: ['SCRAM-SHA-256']});
    checkUser('user', 'passAll', false, true);

    // Fail when mechanisms is not a subset of the current user.
    assert.throws(function() {
        test.updateUser('user', {mechanisms: ['SCRAM-SHA-1']});
    });

    // Fail when passing an empty mechanisms field.
    assert.throws(function() {
        test.updateUser('user', {pwd: 'passEmpty', mechanisms: []});
    });

    // Fail when passing a non-normalized username and producing SHA-256 creds.
    assert.throws(function() {
        test.createUser({user: "\u2168", pwd: 'pass', roles: jsTest.basicUserRoles});
    });

    // Succeed if we're using SHA-1 only.
    test.createUser(
        {user: "\u2168", pwd: 'pass', roles: jsTest.basicUserRoles, mechanisms: ['SCRAM-SHA-1']});
    checkUser("\u2168", 'pass', true, false);

    // Then fail again trying to promote that user to SHA-256.
    assert.throws(function() {
        test.updateUser("\u2168", {pwd: 'pass'});
    });

    MongoRunner.stopMongod(mongod);
})();
