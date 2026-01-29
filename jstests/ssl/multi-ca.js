// Test that servers can use multiple root CAs.

// "root-and-trusted-ca.pem" contains the combined ca.pem and trusted-ca.pem certs.
// This *should* permit client.pem or trusted-client.pem to connect equally.
const CA_CERT = getX509Path("root-and-trusted-ca.pem");
const SERVER_CERT = getX509Path("server.pem");

const CLIENT_CA_CERT = getX509Path("ca.pem");
const CLIENT_CERT = getX509Path("client.pem");
const TRUSTED_CLIENT_CERT = getX509Path("trusted-client.pem");

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
});

function testConnect(cert) {
    const mongo = runMongoProgram(
        "mongo",
        "--host",
        "localhost",
        "--port",
        mongod.port,
        "--tls",
        "--tlsCAFile",
        CLIENT_CA_CERT,
        "--tlsCertificateKeyFile",
        cert,
        "--eval",
        ";",
    );

    assert.eq(0, mongo, "Connection attempt failed using " + cert);
}

testConnect(CLIENT_CERT);
testConnect(TRUSTED_CLIENT_CERT);

MongoRunner.stopMongod(mongod);
