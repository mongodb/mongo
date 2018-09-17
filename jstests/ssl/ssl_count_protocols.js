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
            useLogFiles: true,
            tlsLogVersions: "TLS1_0,TLS1_1,TLS1_2",
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

        print(`Checking ${conn.fullOptions.logFile} for TLS version message`);
        const log = cat(conn.fullOptions.logFile);

        // Find the last line in the log file and verify it has the right version
        let re = /Accepted connection with TLS Version (1\.\d) from connection 127.0.0.1:\d+/g;
        let result = re.exec(log);
        let lastResult = null;
        while (result !== null) {
            lastResult = result;
            result = re.exec(log);
        }

        assert(lastResult !== null,
               "'Accepted connection with TLS Version' log line missing in log file!\n" +
                   "Log file contents: " + conn.fullOptions.logFile +
                   "\n************************************************************\n" + log +
                   "\n************************************************************");

        assert.eq(lastResult['1'], version_number);

        MongoRunner.stopMongod(conn);
    }

    runTestWithoutSubset("TLS1_0");
    runTestWithoutSubset("TLS1_1");
    runTestWithoutSubset("TLS1_2");

})();
