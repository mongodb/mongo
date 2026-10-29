/**
 * Tests that cluster members can authenticate via MONGODB-X509 even when it is excluded from
 * authenticationMechanisms, while external clients are correctly rejected.
 *
 * @tags: [requires_ssl]
 */

const SERVER_CERT = getX509Path("server.pem");
const CLIENT_CERT = getX509Path("client.pem");
const CA_CERT = getX509Path("ca.pem");

// RFC 2253 subject DN of client.pem — the shell's default client certificate.
const CLIENT_USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsAllowInvalidHostnames: "",
    clusterAuthMode: "x509",
    // MONGODB-X509 is deliberately excluded. Cluster members must still get through.
    setParameter: {authenticationMechanisms: "SCRAM-SHA-256"},
});

function connect(certFile) {
    return new Mongo(mongod.host, undefined, {
        tls: {
            certificateKeyFile: certFile,
            CAFile: CA_CERT,
            allowInvalidCertificates: false,
            allowInvalidHostnames: true,
        },
    });
}

// Cluster member (server.pem): both saslStart and authenticate must succeed even though
// MONGODB-X509 is excluded from authenticationMechanisms.
const memberConn = connect(SERVER_CERT);
const memberExternal = memberConn.getDB("$external");

jsTest.log.info("Checking saslStart via cluster-member connection (server.pem)");
assert.commandWorked(
    memberExternal.runCommand({
        saslStart: 1,
        mechanism: "MONGODB-X509",
        payload: BinData(0, ""),
    }),
);

jsTest.log.info("Checking authenticate via cluster-member connection (server.pem)");
assert(memberExternal.auth({mechanism: "MONGODB-X509"}));
memberExternal.logout();

// External client (client.pem): not a cluster member, so the allow-list check must reject it.
const clientConn = connect(CLIENT_CERT);
const clientExternal = clientConn.getDB("$external");

jsTest.log.info("Checking saslStart via external-client connection (client.pem)");
assert.commandFailedWithCode(
    clientExternal.runCommand({
        saslStart: 1,
        mechanism: "MONGODB-X509",
        payload: BinData(0, ""),
    }),
    ErrorCodes.AuthenticationFailed,
);

jsTest.log.info("Checking authenticate via external-client connection (client.pem)");
assert.commandFailedWithCode(
    clientExternal.runCommand({authenticate: 1, mechanism: "MONGODB-X509", user: CLIENT_USER}),
    ErrorCodes.AuthenticationFailed,
);

MongoRunner.stopMongod(mongod);
