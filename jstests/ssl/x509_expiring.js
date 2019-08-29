// Verify a warning is emitted when a certificate is about to expire.

(function() {
'use strict';

const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

function test(expiration, expect) {
    const options = {
        auth: '',
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        setParameter: 'tlsX509ExpirationWarningThresholdDays=' + expiration,
    };
    const mongo = MongoRunner.runMongod(options);
    const external = mongo.getDB("$external");

    external.createUser({
        user: CLIENT_USER,
        roles: [
            {'role': 'userAdminAnyDatabase', 'db': 'admin'},
            {'role': 'readWriteAnyDatabase', 'db': 'admin'},
            {'role': 'clusterMonitor', 'db': 'admin'},
        ]
    });

    assert(external.auth({user: CLIENT_USER, mechanism: 'MONGODB-X509'}),
           "authentication with valid user failed");

    // Check that there's a "Successfully authenticated" message that includes the client IP
    const log =
        assert.commandWorked(external.getSiblingDB("admin").runCommand({getLog: "global"})).log;
    const warning = `Peer certificate '${CLIENT_USER}' expires`;

    assert.eq(log.some(line => line.includes(warning)), expect);

    MongoRunner.stopMongod(mongo);
}

test(30, false);
test(7300, true);  // Work so long as certs expire no more than 20 years from now
})();
