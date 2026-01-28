let CA_CERT = getX509Path("ca.pem");
let SERVER_CERT = getX509Path("server.pem");
let CLIENT_CERT = getX509Path("client.pem");
let BAD_SAN_CERT = getX509Path("badSAN.pem");

let mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsClusterFile: BAD_SAN_CERT,
});

let mongo = runMongoProgram(
    "mongo",
    "--host",
    "localhost",
    "--port",
    mongod.port,
    "--tls",
    "--tlsCAFile",
    CA_CERT,
    "--tlsCertificateKeyFile",
    CLIENT_CERT,
    "--eval",
    ";",
);

// runMongoProgram returns 0 on success
assert.eq(0, mongo, "Connection attempt failed when an irrelevant tlsClusterFile was provided to the server!");
MongoRunner.stopMongod(mongod);
