// Test for speculativeAuthenticate during isMaster.

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

admin.createUser(
    {user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-256']});
admin.auth('admin', 'pwd');

const X509USER = 'CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';
external.createUser({user: X509USER, roles: [{role: 'root', db: 'admin'}]});

function test(uri) {
    const x509 = runMongoProgram('mongo',
                                 '--tls',
                                 '--tlsCAFile',
                                 'jstests/libs/ca.pem',
                                 '--tlsCertificateKeyFile',
                                 'jstests/libs/client.pem',
                                 uri,
                                 '--eval',
                                 ';');
    assert.eq(0, x509);
}

function assertStats(cb) {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                          .security.authentication.mechanisms;
    cb(mechStats);
}

// No speculative auth attempts yet.
assertStats(function(mechStats) {
    Object.keys(mechStats).forEach(function(mech) {
        const stats = mechStats[mech].speculativeAuthenticate;
        assert.eq(stats.received, 0);
        assert.eq(stats.successful, 0);
    });
});

// Connect with speculation and have 1/1 result.
const baseURI = 'mongodb://localhost:' + mongod.port + '/admin';
test(baseURI + '?authMechanism=MONGODB-X509');
assertStats(function(mechStats) {
    const stats = mechStats['MONGODB-X509'].speculativeAuthenticate;
    assert.eq(stats.received, 1);
    assert.eq(stats.successful, 1);
});

// Connect without speculation and still have 1/1 result.
test(baseURI);
assertStats(function(mechStats) {
    const stats = mechStats['MONGODB-X509'].speculativeAuthenticate;
    assert.eq(stats.received, 1);
    assert.eq(stats.successful, 1);
});

MongoRunner.stopMongod(mongod);
})();
