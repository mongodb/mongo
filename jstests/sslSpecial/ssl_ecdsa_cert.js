load('jstests/ssl/libs/ssl_helpers.js');

const test = () => {
    "use strict";

    const ECDSA_CA_CERT = 'jstests/libs/ecdsa-ca.pem';
    const ECDSA_CLIENT_CERT = 'jstests/libs/ecdsa-client.pem';
    const ECDSA_SERVER_CERT = 'jstests/libs/ecdsa-server.pem';

    const CLIENT_USER = 'CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US';

    print('Testing if platform supports usage of ECDSA certificates');
    const tlsOptions = {
        tlsMode: 'preferTLS',
        tlsCertificateKeyFile: ECDSA_SERVER_CERT,
        tlsCAFile: ECDSA_CA_CERT,
        ipv6: '',
        bind_ip_all: '',
        waitForConnect: true,
        tlsAllowConnectionsWithoutCertificates: "",
    };

    let mongod = MongoRunner.runMongod(tlsOptions);

    // Verify we can connect
    assert.eq(0,
              runMongoProgram('mongo',
                              '--tls',
                              '--tlsCAFile',
                              ECDSA_CA_CERT,
                              '--port',
                              mongod.port,
                              '--eval',
                              'db.isMaster()'),
              "mongo did not initialize properly");

    // Add an X509 user
    const addUserCmd = {createUser: CLIENT_USER, roles: [{role: 'root', db: 'admin'}]};
    assert.commandWorked(mongod.getDB('$external').runCommand(addUserCmd),
                         'Failed to create X509 user using ECDSA certificates');

    const command = function() {
        assert(db.getSiblingDB('$external').auth({mechanism: 'MONGODB-X509', user: "CLIENT_USER"}));

        const connStatus = db.getSiblingDB('admin').runCommand({connectionStatus: 1});
        assert(connStatus.authInfo.authenticatedUsers[0].user === "CLIENT_USER");
    };

    // Verify we can authenticate via X509
    assert.eq(
        0,
        runMongoProgram('mongo',
                        '--tls',
                        '--tlsCertificateKeyFile',
                        ECDSA_CLIENT_CERT,
                        '--tlsCAFile',
                        ECDSA_CA_CERT,
                        '--port',
                        mongod.port,
                        '--eval',
                        '(' + command.toString().replace(/CLIENT_USER/g, CLIENT_USER) + ')();'),
        "ECDSA X509 authentication failed");
    MongoRunner.stopMongod(mongod);
};

const EXCLUDED_BUILDS = ['amazon', 'amzn64'];
if (EXCLUDED_BUILDS.includes(buildInfo().buildEnvironment.distmod)) {
    print("*****************************************************");
    print("Skipping test because Amazon Linux does not support ECDSA certificates");
    print("*****************************************************");
} else {
    requireSSLProvider('openssl', test);
}
