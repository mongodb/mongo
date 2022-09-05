// Ensure the server counts the server TLS versions used
(function() {
'use strict';

load("jstests/ssl/libs/ssl_helpers.js");

var SERVER_CERT = "jstests/libs/server.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var CA_CERT = "jstests/libs/ca.pem";

const protocols = ["TLS1_0", "TLS1_1", "TLS1_2", "TLS1_3"];

// First, figure out what protocol our local TLS stack wants to speak.
// We're going to observe a connection of this type from the testrunner.
const expectedDefaultProtocol = detectDefaultTLSProtocol();
print("Expected default protocol: " + expectedDefaultProtocol);

function runTestWithoutSubset(client) {
    print("Running test: " + client);
    let disabledProtocols = protocols.slice();
    let expectedCounts = [0, 0, 0, 0, 0];
    expectedCounts[protocols.indexOf(expectedDefaultProtocol)] = 1;
    var index = disabledProtocols.indexOf(client);
    disabledProtocols.splice(index, 1);
    expectedCounts[index] += 1;
    print(tojson(expectedCounts));

    const conn = MongoRunner.runMongod({
        sslMode: 'allowSSL',
        sslPEMKeyFile: SERVER_CERT,
        sslDisabledProtocols: 'none',
        useLogFiles: true,
        tlsLogVersions: "TLS1_0,TLS1_1,TLS1_2,TLS1_3",
    });

    print(disabledProtocols);
    const version_number = client.replace(/TLS/, "").replace(/_/, ".");

    const exitStatus = runMongoProgram('mongo',
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
                                       // The Javascript string "1.0" is implicitly converted to the
                                       // Number(1) Workaround this with parseFloat
                                       'one = Number.parseFloat(1).toPrecision(2); a = {};' +
                                           'a[one] = NumberLong(' + expectedCounts[0] + ');' +
                                           'a["1.1"] = NumberLong(' + expectedCounts[1] + ');' +
                                           'a["1.2"] = NumberLong(' + expectedCounts[2] + ');' +
                                           'a["1.3"] = NumberLong(' + expectedCounts[3] + ');' +
                                           'a["unknown"] = NumberLong(' + expectedCounts[4] + ');' +
                                           'assert.eq(db.serverStatus().transportSecurity, a);');

    if (expectedDefaultProtocol === "TLS1_2" && client === "TLS1_3") {
        // If the runtime environment does not support TLS 1.3, a client cannot connect to a
        // server if TLS 1.3 is its only usable protocol version.
        assert.neq(0,
                   exitStatus,
                   "A client which does not support TLS 1.3 should not be able to connect with it");
        MongoRunner.stopMongod(conn);
        return;
    }

    assert.eq(0, exitStatus, "");

    print(`Checking ${conn.fullOptions.logFile} for TLS version message`);
    const log = cat(conn.fullOptions.logFile);

    const lines = log.split('\n');
    let found = false;
    for (let logMsg of lines) {
        if (!logMsg) {
            continue;
        }
        const logJson = JSON.parse(logMsg);
        if (logJson.id === 23218 && /1\.\d/.test(logJson.attr.tlsVersion) &&
            /127.0.0.1:\d+/.test(logJson.attr.remoteHost)) {
            found = true;
            break;
        }
    }
    assert(found,
           "'Accepted connection with TLS Version' log line missing in log file!\n" +
               "Log file contents: " + conn.fullOptions.logFile +
               "\n************************************************************\n" + log +
               "\n************************************************************");

    MongoRunner.stopMongod(conn);
}
if (sslProviderSupportsTLS1_0()) {
    runTestWithoutSubset("TLS1_0");
}
if (sslProviderSupportsTLS1_1()) {
    runTestWithoutSubset("TLS1_1");
}
runTestWithoutSubset("TLS1_2");
runTestWithoutSubset("TLS1_3");
})();
