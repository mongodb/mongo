/**
 * Test that port is validated when starting a mongod with gRPC enabled.
 *
 * Despite the fact that this tests gRPC, the gRPC build variant messes with the port options set in
 * the test causing this test to fail on the gRPC build variants.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */
if (_isWindows()) {
    quit();
}

import {
    CA_CERT,
    CLIENT_CERT,
    SERVER_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";

function runTest(port, expectedError) {
    clearRawMongoProgramOutput();

    let conn;
    try {
        conn = MongoRunner.runMongod({
            grpcPort: port,
            tlsMode: "preferTLS",
            tlsCertificateKeyFile: SERVER_CERT,
            tlsCAFile: CA_CERT,
            setParameter: {
                featureFlagGRPC: true,
            },
        });
    } catch (e) {
        const logContents = rawMongoProgramOutput("\"id\":20574");
        assert.neq(logContents.match(expectedError),
                   null,
                   "did not see expected error message in log output: " + expectedError);
        return;
    }

    const evalFunc = function(dbname, cmd) {
        jsTest.log(assert.commandWorked(db.getSiblingDB(dbname).runCommand({ping: 1})));
    };
    const evalstr = `(${evalFunc})(${tojson('admin')}, ${tojson({ping: 1})});`;

    let exitCode;
    try {
        exitCode = runMongoProgram("mongo",
                                   "--gRPC",
                                   "--port",
                                   port,
                                   "--tls",
                                   "--tlsCAFile",
                                   CA_CERT,
                                   "--tlsCertificateKeyFile",
                                   CLIENT_CERT,
                                   '--eval',
                                   evalstr);
    } catch (e) {
        jsTest.log("Caught exception while running ping command over gRPC: " + e);
    }

    assert.eq(exitCode, 0, "Failed to run ping command over gRPC");

    MongoRunner.stopMongod(conn);
}

const expectedGteError = /"errmsg":"net.grpc.port must be greater than or equal to 1"/;
const expectedLteError = /"errmsg":"net.grpc.port must be less than or equal to 65535"/;
runTest(0, expectedGteError);
runTest(allocatePort(), null);
runTest(65536, expectedLteError);
