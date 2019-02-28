load('jstests/ssl/libs/ssl_helpers.js');

(function() {
    "use strict";

    const SERVER1_CERT = "jstests/libs/server_SAN.pem";
    const SERVER2_CERT = "jstests/libs/server_SAN2.pem";
    const CA_CERT = "jstests/libs/ca.pem";
    const CLIENT_CERT = "jstests/libs/client.pem";

    // Some test machines lack ipv6 so test for by starting a mongod that needs to bind to an ipv6
    // address.
    var hasIpv6 = true;
    const mongodHasIpv6 = MongoRunner.runMongod({
        sslMode: "requireSSL",
        sslPEMKeyFile: SERVER1_CERT,
        sslCAFile: CA_CERT,
        ipv6: "",
        bind_ip: "::1,127.0.0.1"
    });
    if (mongodHasIpv6 == null) {
        jsTest.log("Unable to run all tests because ipv6 is not on machine, see BF-10990");
        hasIpv6 = false;
    } else {
        MongoRunner.stopMongod(mongodHasIpv6);
    }

    function authAndTest(cert_option) {
        function test_host(host, port) {
            let args = [
                "mongo",
                "--host",
                host,
                "--port",
                port,
                "--ssl",
                "--sslCAFile",
                CA_CERT,
                "--sslPEMKeyFile",
                CLIENT_CERT,
                "--eval",
                ";"
            ];

            if (hasIpv6) {
                args.push("--ipv6");
            }

            const mongo = runMongoProgram.apply(null, args);

            assert.eq(0, mongo, "Connection succeeded");
        }

        const x509_options = {sslMode: "requireSSL", sslCAFile: CA_CERT, bind_ip_all: ""};

        if (hasIpv6) {
            Object.extend(x509_options, {ipv6: ""});
        }

        let mongod = MongoRunner.runMongod(Object.merge(x509_options, cert_option));

        test_host("localhost", mongod.port);
        test_host("127.0.0.1", mongod.port);
        if (hasIpv6) {
            test_host("::1", mongod.port);
        }

        MongoRunner.stopMongod(mongod);
    }

    print("1. Test parsing different values in SAN DNS and IP fields. ");
    authAndTest({sslPEMKeyFile: SERVER1_CERT});
    print("2. Test parsing IP Addresses in SAN DNS fields. ");
    authAndTest({sslPEMKeyFile: SERVER2_CERT});

}());
