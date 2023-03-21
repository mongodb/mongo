// Test configuration parameter tlsClusterAuthX509ExtensionValue
// aka: net.tls.clusterAuthX509.extensionValue
// @tags: [ featureFlagConfigurableX509ClusterAuthn ]

(function() {
'use strict';

load('jstests/ssl/libs/ssl_helpers.js');
if (determineSSLProvider() !== "openssl") {
    jsTest.log('Test requires openssl based TLS support');
    return;
}

// Fails when used without clusterAuthMode == 'X509'
{
    const opts = {auth: '', tlsClusterAuthX509ExtensionValue: 'foo'};
    const errmsg =
        'net.tls.clusterAuthX509.extensionValue requires a clusterAuthMode which allows for usage of X509';

    jsTest.log('No clusterAuthMode set');
    clearRawMongoProgramOutput();
    assert.throws(() => MongoRunner.runMongod(opts));
    assert(rawMongoProgramOutput().includes(errmsg));

    jsTest.log('clusterAuthMode == keyFile');
    clearRawMongoProgramOutput();
    opts.clusterAuthMode = 'keyFile';
    assert.throws(() => MongoRunner.runMongod(opts));
    assert(rawMongoProgramOutput().includes(errmsg));
}

function authAndDo(port, cert, cmd = ';') {
    jsTest.log('Connecting to localhost using cert: ' + cert);
    function x509auth(db) {
        const ext = db.getSiblingDB('$external');
        assert.commandWorked(ext.runCommand({authenticate: 1, mechanism: 'MONGODB-X509'}));
        return ext.adminCommand({connectionStatus: 1});
    }
    clearRawMongoProgramOutput();
    const shell = runMongoProgram('mongo',
                                  '--host',
                                  'localhost',
                                  '--port',
                                  port,
                                  '--tls',
                                  '--tlsCAFile',
                                  'jstests/libs/ca.pem',
                                  '--tlsCertificateKeyFile',
                                  cert,
                                  '--eval',
                                  x509auth + ' x509auth(db); ' + cmd);
    assert.eq(shell, 0);
}

function runTest(conn) {
    const SERVER_RDN = 'CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US';
    const SERVER = 'jstests/libs/server.pem';
    const FOO_MEMBER = 'jstests/ssl/libs/cluster-member-foo.pem';
    const BAR_MEMBER = 'jstests/ssl/libs/cluster-member-bar.pem';
    const FOO_MEMBER_ALT = 'jstests/ssl/libs/cluster-member-foo-alt-rdn.pem';
    const FOO_MEMBER_ALT_RDN = 'CN=Doer,OU=Business,O=Company,L=Fakesville,ST=Example,C=ZZ';

    const admin = conn.getDB('admin');
    const ext = conn.getDB('$external');

    // Ensure no localhost auth bypass available.
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));
    assert(admin.auth('admin', 'admin'));

    // Connect using server.pem which has the same RDN, but no custom extension.
    // This will result in an unknown user condition because we are
    // not recognized as a cluster member.
    assert.throws(() => authAndDo(conn.port, SERVER));

    const insertCmd = 'assert.writeOK(db.getSiblingDB("test").mycoll.insert({x:1}));';
    // Connect using same RDN WITH custom extension.
    authAndDo(conn.port, FOO_MEMBER, insertCmd);

    // Connect using cert with membership extension, but wrong value.
    assert.throws(() => authAndDo(conn.port, BAR_MEMBER));

    // Connect using cert with right membership, but different RDN (allowed).
    authAndDo(conn.port, FOO_MEMBER_ALT, insertCmd);

    // Create a user who would have been a cluster member under name based rules.
    // We should have basic privs, testing with read but not write.
    const readCmd = 'db.getSiblingDB("test").mycoll.find({});';
    const readRoles = [{db: 'admin', role: 'readAnyDatabase'}];
    assert.commandWorked(ext.runCommand({createUser: SERVER_RDN, roles: readRoles}));
    authAndDo(conn.port, SERVER, readCmd);
    assert.throws(() => authAndDo(conn.port, SERVER, insertCmd));

    // Create a user with FOO_MEMBER_ALT's RDN to validate enforceUserClusterSeparation.
    authAndDo(conn.port, FOO_MEMBER_ALT);
    assert.commandWorked(ext.runCommand({createUser: FOO_MEMBER_ALT_RDN, roles: readRoles}));
    assert.throws(() => authAndDo(conn.port, FOO_MEMBER_ALT));
}

{
    const opts = {
        auth: '',
        tlsMode: 'requireTLS',
        tlsCertificateKeyFile: 'jstests/ssl/libs/cluster-member-foo.pem',
        tlsCAFile: 'jstests/libs/ca.pem',
        clusterAuthMode: 'x509',
        tlsClusterAuthX509ExtensionValue: 'foo',
        setParameter: {
            enforceUserClusterSeparation: 'true',
        },
    };

    const mongod = MongoRunner.runMongod(opts);
    runTest(mongod);
    MongoRunner.stopMongod(mongod);
}
})();
