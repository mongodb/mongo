// Test that clients support "BEGIN PRIVATE KEY" pems with RSA keys
const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";
const CLIENT_CERT = "jstests/libs/client_privatekey.pem";

function authAndTest(port) {
    const mongo = runMongoProgram("mongo",
                                  "--host",
                                  "localhost",
                                  "--port",
                                  port,
                                  "--tls",
                                  "--tlsCAFile",
                                  CA_CERT,
                                  "--tlsCertificateKeyFile",
                                  CLIENT_CERT,
                                  "--eval",
                                  "1");

    // runMongoProgram returns 0 on success
    assert.eq(0, mongo, "Connection attempt failed");
}

const x509_options = {
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT
};

let mongo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));

authAndTest(mongo.port);

MongoRunner.stopMongod(mongo);