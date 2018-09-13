load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    const SERVER1_CERT = "jstests/libs/server_SAN.pem";
    const SERVER2_CERT = "jstests/libs/server_SAN2.pem";
    const CA_CERT = "jstests/libs/ca.pem";
    const CLIENT_CERT = "jstests/libs/client.pem";

    function authAndTest(cert_option) {
        function test_host(host, port) {
            const mongo = runMongoProgram("mongo",
                                          "--host",
                                          host,
                                          "--port",
                                          port,
                                          "--ipv6",
                                          "--ssl",
                                          "--sslCAFile",
                                          CA_CERT,
                                          "--sslPEMKeyFile",
                                          CLIENT_CERT,
                                          "--eval",
                                          ";");

            assert.eq(0, mongo, "Connection succeeded");
        }

        const x509_options = {sslMode: "requireSSL", sslCAFile: CA_CERT, ipv6: "", bind_ip_all: ""};

        let mongod = MongoRunner.runMongod(Object.merge(x509_options, cert_option));

        test_host("localhost", mongod.port);
        test_host("127.0.0.1", mongod.port);
        test_host("::1", mongod.port);

        MongoRunner.stopMongod(mongod);
    }

    print("1. Test parsing different values in SAN DNS and IP fields. ");
    authAndTest({sslPEMKeyFile: SERVER1_CERT});
    print("2. Test parsing IP Addresses in SAN DNS fields. ");
    authAndTest({sslPEMKeyFile: SERVER2_CERT});

}());
