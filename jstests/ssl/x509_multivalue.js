// Test X509 auth with custom OIDs.

const SERVER_CERT = "jstests/libs/server.pem";
const CA_CERT = "jstests/libs/ca.pem";

function testClient(conn, name) {
    let auth = {mechanism: "MONGODB-X509"};
    if (name !== null) {
        auth.name = name;
    }
    const script = "assert(db.getSiblingDB('$external').auth(" + tojson(auth) + "));";
    clearRawMongoProgramOutput();
    const exitCode = runMongoProgram(
        "mongo",
        "--tls",
        "--tlsAllowInvalidHostnames",
        "--tlsCertificateKeyFile",
        "jstests/libs/client-multivalue-rdn.pem",
        "--tlsCAFile",
        CA_CERT,
        "--port",
        conn.port,
        "--eval",
        script,
    );

    assert.eq(exitCode, 0);
}

function runTest(conn) {
    const NAME = "L=New York City+ST=New York+C=US,OU=KernelUser+O=MongoDB+CN=client";

    const admin = conn.getDB("admin");
    admin.createUser({user: "admin", pwd: "admin", roles: ["root"]});
    admin.auth("admin", "admin");

    const external = conn.getDB("$external");
    external.createUser({user: NAME, roles: [{"role": "readWrite", "db": "test"}]});

    testClient(conn, NAME);
    testClient(conn, null);
}

// Standalone.
const mongod = MongoRunner.runMongod({
    auth: "",
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsAllowInvalidCertificates: "",
});
runTest(mongod);
MongoRunner.stopMongod(mongod);
