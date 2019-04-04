// Test that clients support "BEGIN PRIVATE KEY" pems with RSA keys
load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    const SERVER_CERT = "jstests/libs/server.pem";
    const CA_CERT = "jstests/libs/ca.pem";
    const CLIENT_CERT = "jstests/libs/client_privatekey.pem";

    function authAndTest(port) {
        const merizo = runMongoProgram("merizo",
                                      "--host",
                                      "localhost",
                                      "--port",
                                      port,
                                      "--ssl",
                                      "--sslCAFile",
                                      CA_CERT,
                                      "--sslPEMKeyFile",
                                      CLIENT_CERT,
                                      "--eval",
                                      "1");

        // runMongoProgram returns 0 on success
        assert.eq(0, merizo, "Connection attempt failed");
    }

    const x509_options = {sslMode: "requireSSL", sslPEMKeyFile: SERVER_CERT, sslCAFile: CA_CERT};

    let merizo = MongoRunner.runMongod(Object.merge(x509_options, {auth: ""}));

    authAndTest(merizo.port);

    MongoRunner.stopMongod(merizo);
}());
