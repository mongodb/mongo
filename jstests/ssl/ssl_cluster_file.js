var CA_CERT = "jstests/libs/ca.pem";
var SERVER_CERT = "jstests/libs/server.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var BAD_SAN_CERT = "jstests/libs/badSAN.pem";

var mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    tlsClusterFile: BAD_SAN_CERT
});

var mongo = runMongoProgram("mongo",
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
                            ";");

// runMongoProgram returns 0 on success
assert.eq(
    0,
    mongo,
    "Connection attempt failed when an irrelevant tlsClusterFile was provided to the server!");
MongoRunner.stopMongod(mongod);
