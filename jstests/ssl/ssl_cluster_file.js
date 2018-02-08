(function() {
    "use strict";

    var CA_CERT = "jstests/libs/ca.pem";
    var SERVER_CERT = "jstests/libs/server.pem";
    var CLIENT_CERT = "jstests/libs/client.pem";
    var BAD_SAN_CERT = "jstests/libs/badSAN.pem";

    var mongod = MongoRunner.runMongod({
        sslMode: "requireSSL",
        sslPEMKeyFile: SERVER_CERT,
        sslCAFile: CA_CERT,
        sslClusterFile: BAD_SAN_CERT
    });

    var mongo = runMongoProgram("mongo",
                                "--host",
                                "localhost",
                                "--port",
                                mongod.port,
                                "--ssl",
                                "--sslCAFile",
                                CA_CERT,
                                "--sslPEMKeyFile",
                                CLIENT_CERT,
                                "--eval",
                                ";");

    // runMongoProgram returns 0 on success
    assert.eq(
        0,
        mongo,
        "Connection attempt failed when an irrelevant sslClusterFile was provided to the server!");
    MongoRunner.stopMongod(mongod);
}());
