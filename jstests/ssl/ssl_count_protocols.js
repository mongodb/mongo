// Ensure the server counts the server TLS versions used
(function() {
    'use strict';

    var SERVER_CERT = "jstests/libs/server.pem";
    var CLIENT_CERT = "jstests/libs/client.pem";
    var CA_CERT = "jstests/libs/ca.pem";

    function runTestWithoutSubset(client) {
        let disabledProtocols = ["TLS1_0", "TLS1_1", "TLS1_2"];
        let expectedCounts = [0, 0, 1];
        var index = disabledProtocols.indexOf(client);
        disabledProtocols.splice(index, 1);
        expectedCounts[index] += 1;

        const conn = MongoRunner.runMongod({
            sslMode: 'allowSSL',
            sslPEMKeyFile: SERVER_CERT,
            sslDisabledProtocols: 'none',
        });

        print(disabledProtocols);
        const version_number = client.replace(/TLS/, "").replace(/_/, ".");

        const exitStatus =
            runMongoProgram('mongo',
                            '--ssl',
                            '--sslAllowInvalidHostnames',
                            '--sslPEMKeyFile',
                            CLIENT_CERT,
                            '--sslCAFile',
                            CA_CERT,
                            '--port',
                            conn.port,
                            '--sslDisabledProtocols',
                            disabledProtocols.join(","),
                            '--eval',
                            // The Javascript string "1.0" is implicitly converted to the Number(1)
                            // Workaround this with parseFloat
                            'one = Number.parseFloat(1).toPrecision(2); a = {};' +
                                'a[one] = NumberLong(' + expectedCounts[0] + ');' +
                                'a["1.1"] = NumberLong(' + expectedCounts[1] + ');' +
                                'a["1.2"] = NumberLong(' + expectedCounts[2] + ');' +
                                'assert.eq(db.serverStatus().transportSecurity, a);');

        assert.eq(0, exitStatus, "");

        MongoRunner.stopMongod(conn);
    }

    runTestWithoutSubset("TLS1_0");
    runTestWithoutSubset("TLS1_1");
    runTestWithoutSubset("TLS1_2");

})();
