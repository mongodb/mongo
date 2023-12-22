// Test X509 auth with custom OIDs.

function testClient(conn, name) {
    let auth = {mechanism: 'MONGODB-X509'};
    if (name !== null) {
        auth.name = name;
    }
    const script = 'assert(db.getSiblingDB(\'$external\').auth(' + tojson(auth) + '));';
    clearRawMongoProgramOutput();
    const exitCode = runMongoProgram('mongo',
                                     '--tls',
                                     '--tlsAllowInvalidHostnames',
                                     '--tlsCertificateKeyFile',
                                     'jstests/libs/client-custom-oids.pem',
                                     '--tlsCAFile',
                                     'jstests/libs/ca.pem',
                                     '--port',
                                     conn.port,
                                     '--eval',
                                     script);

    assert.eq(exitCode, 0);
}

function runTest(conn) {
    const NAME =
        '1.2.3.45=Value\\,Rando,1.2.3.56=RandoValue,CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';
    const admin = conn.getDB('admin');
    admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    admin.auth('admin', 'admin');

    const external = conn.getDB('$external');
    external.createUser({user: NAME, roles: [{'role': 'readWrite', 'db': 'test'}]});

    testClient(conn, NAME);
    testClient(conn, null);
}

// Standalone.
const mongod = MongoRunner.runMongod({
    auth: '',
    tlsMode: 'requireTLS',
    // Server PEM file is server.pem to match the shell's ca.pem.
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
    // Server CA file is non-expiring-ca.pem to match the shell's client-custom-oids.pem.
    tlsCAFile: 'jstests/libs/non-expiring-ca.pem',
    tlsAllowInvalidCertificates: '',
});
runTest(mongod);
MongoRunner.stopMongod(mongod);