// Ensure that the shell may connect to servers running supporting restricted subsets of TLS
// protocols.

var SERVER_CERT = "jstests/libs/server.pem";
var CLIENT_CERT = "jstests/libs/client.pem";
var CA_CERT = "jstests/libs/ca.pem";

function runTestWithoutSubset(subset) {
    const disabledProtocols = subset.join(",");
    const conn = MongoRunner.runMongod({
        tlsMode: 'allowTLS',
        tlsCertificateKeyFile: SERVER_CERT,
        tlsDisabledProtocols: disabledProtocols,
        tlsCAFile: CA_CERT
    });

    const exitStatus = runMongoProgram('mongo',
                                       '--tls',
                                       '--tlsAllowInvalidHostnames',
                                       '--tlsCertificateKeyFile',
                                       CLIENT_CERT,
                                       '--tlsCAFile',
                                       CA_CERT,
                                       '--port',
                                       conn.port,
                                       '--eval',
                                       'quit()');

    assert.eq(0, exitStatus, "");

    MongoRunner.stopMongod(conn);
}

runTestWithoutSubset(["TLS1_0"]);
runTestWithoutSubset(["TLS1_2"]);
runTestWithoutSubset(["TLS1_0", "TLS1_1"]);