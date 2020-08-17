// Test for auth counters using MONGODB-X509.

(function() {
'use strict';

const x509 = "MONGODB-X509";
const mongod = MongoRunner.runMongod({
    auth: '',
    tlsMode: 'requireTLS',
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
    tlsCAFile: 'jstests/libs/ca.pem',
    clusterAuthMode: "x509",
});
const admin = mongod.getDB('admin');
const external = mongod.getDB('$external');

admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
admin.auth('admin', 'pwd');

const X509USER = 'CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';
external.createUser({user: X509USER, roles: []});

// This test ignores counters for SCRAM-SHA-*.
// For those, see jstests/auth/auth-counters.js
const expected = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                     .security.authentication.mechanisms[x509];

function assertStats() {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                          .security.authentication.mechanisms[x509];
    try {
        assert.eq(mechStats.authenticate.received, expected.authenticate.received);
        assert.eq(mechStats.authenticate.successful, expected.authenticate.successful);
        assert.eq(mechStats.clusterAuthenticate.received, expected.clusterAuthenticate.received);
        assert.eq(mechStats.clusterAuthenticate.successful,
                  expected.clusterAuthenticate.successful);
    } catch (e) {
        print("mechStats: " + tojson(mechStats));
        print("expected: " + tojson(expected));
        throw e;
    }
}

function assertSuccess(creds) {
    assert.eq(external.auth(creds), true);
    external.logout();
    ++expected.authenticate.received;
    ++expected.authenticate.successful;
    assertStats();
}

function assertFailure(creds) {
    assert.eq(external.auth(creds), false);
    ++expected.authenticate.received;
    assertStats();
}

function assertSuccessInternal() {
    assert.eq(runMongoProgram("mongo",
                              "--tls",
                              "--port",
                              mongod.port,
                              "--tlsCertificateKeyFile",
                              "jstests/libs/server.pem",
                              "--tlsCAFile",
                              "jstests/libs/ca.pem",
                              "--authenticationDatabase",
                              "$external",
                              "--authenticationMechanism",
                              "MONGODB-X509",
                              "--eval",
                              ";"),
              0);
    ++expected.authenticate.received;
    ++expected.authenticate.successful;
    ++expected.clusterAuthenticate.received;
    ++expected.clusterAuthenticate.successful;
    assertStats();
}

// User from certificate should work.
assertSuccess({mechanism: x509});

// Explicitly named user.
assertSuccess({user: X509USER, mechanism: x509});

// Cluster auth counter checks.
// We can't test failures with the __system user without the handshake failing,
// which won't increment the counters.
assertSuccessInternal();

// Fails once the user no longer exists.
external.dropUser(X509USER);
assertFailure({mechanism: x509});

const finalStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
MongoRunner.stopMongod(mongod);
printjson(finalStats);
})();
