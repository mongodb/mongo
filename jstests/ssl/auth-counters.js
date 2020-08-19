// Test for auth counters using MONGODB-X509.

(function() {
'use strict';

const mongod = MongoRunner.runMongod({
    auth: '',
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
    tlsCAFile: 'jstests/libs/ca.pem',
});
const admin = mongod.getDB('admin');
const external = mongod.getDB('$external');

admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
admin.auth('admin', 'pwd');

const X509USER = 'CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';
external.createUser({user: X509USER, roles: []});

// This test ignores counters for SCRAM-SHA-*.
// For those, see jstests/auth/auth-counters.js
const expected = {
    received: 0,
    successful: 0
};

function assertStats() {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                          .security.authentication.mechanisms['MONGODB-X509']
                          .authenticate;
    assert.eq(mechStats.received, expected.received);
    assert.eq(mechStats.successful, expected.successful);
}

function assertSuccess(creds) {
    assert.eq(external.auth(creds), true);
    external.logout();
    ++expected.received;
    ++expected.successful;
    assertStats();
}

function assertFailure(creds) {
    assert.eq(external.auth(creds), false);
    ++expected.received;
    assertStats();
}

// User from certificate should work.
assertSuccess({mechanism: 'MONGODB-X509'});

// Explicitly named user.
assertSuccess({user: X509USER, mechanism: 'MONGODB-X509'});

// Fails once the user no longer exists.
external.dropUser(X509USER);
assertFailure({mechanism: 'MONGODB-X509'});

const finalStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
MongoRunner.stopMongod(mongod);

printjson(finalStats);
})();
