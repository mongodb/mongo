// Test X509 auth with custom OIDs.

(function() {
    'use strict';

    const SERVER_CERT = 'jstests/libs/server.pem';
    const CA_CERT = 'jstests/libs/ca.pem';

    function testClient(conn, name) {
        let auth = {mechanism: 'MERIZODB-X509'};
        if (name !== null) {
            auth.name = name;
        }
        const script = 'assert(db.getSiblingDB(\'$external\').auth(' + tojson(auth) + '));';
        clearRawMerizoProgramOutput();
        const exitCode = runMerizoProgram('merizo',
                                         '--ssl',
                                         '--sslAllowInvalidHostnames',
                                         '--sslPEMKeyFile',
                                         'jstests/libs/client-multivalue-rdn.pem',
                                         '--sslCAFile',
                                         CA_CERT,
                                         '--port',
                                         conn.port,
                                         '--eval',
                                         script);

        assert.eq(exitCode, 0);
    }

    function runTest(conn) {
        const NAME = 'L=New York City+ST=New York+C=US,OU=KernelUser+O=MerizoDB+CN=client';

        const admin = conn.getDB('admin');
        admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
        admin.auth('admin', 'admin');

        const external = conn.getDB('$external');
        external.createUser({user: NAME, roles: [{'role': 'readWrite', 'db': 'test'}]});

        testClient(conn, NAME);
        testClient(conn, null);
    }

    // Standalone.
    const merizod = MerizoRunner.runMerizod({
        auth: '',
        sslMode: 'requireSSL',
        sslPEMKeyFile: SERVER_CERT,
        sslCAFile: CA_CERT,
        sslAllowInvalidCertificates: '',
    });
    runTest(merizod);
    MerizoRunner.stopMerizod(merizod);
})();
