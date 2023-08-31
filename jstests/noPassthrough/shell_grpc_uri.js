import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const tlsCAFile = "jstests/libs/ca.pem";

// Constructs a new Mongo instance with the provided URI and asserts it fails with the provided
// error code.
function assertConnectFailsWithErrorCode(uri, errorCode) {
    jsTestLog(`Connecting to ${uri}`)
    assert.throwsWithCode(() => new Mongo(uri), errorCode);
}

// Runs a new shell process with the provided arguments and asserts that its exit code matches `ok`
// (true for success). This is used over assertConnectFailsWithErrorCode when CLI-only arguments
// need to be specified.
function testShellConnect(ok, ...args) {
    const exitCode = runMongoProgram('mongo', '--eval', ';', ...args);
    if (ok) {
        assert.eq(exitCode, 0, "failed to connect with `" + args.join(' ') + "`");
    } else {
        assert.neq(exitCode, 0, "unexpectedly succeeded connecting with `" + args.join(' ') + "`");
    }
}

const mongod = MongoRunner.runMongod({
    tlsMode: "allowTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile,
    tlsAllowConnectionsWithoutCertificates: '',
});

if (!FeatureFlagUtil.isPresentAndEnabled(mongod.getDB("admin"), "GRPC")) {
    jsTestLog("Skipping shell_grpc_uri.js test due to featureFlagGRPC being disabled");
    MongoRunner.stopMongod(mongod);
    quit();
}

const host = `localhost:${mongod.port}`;

testShellConnect(true, `mongodb://${host}`, "--gRPC", "--tls", "--tlsCAFile", tlsCAFile);
testShellConnect(true, `mongodb://${host}/?gRPC=true`, "--tls", "--tlsCAFile", tlsCAFile);
testShellConnect(false,
                 `mongodb://${host}`,
                 "--gRPC",
                 "--tls",
                 "--tlsCAFile",
                 tlsCAFile,
                 "--tlsCertificateKeyFile",
                 "jstests/libs/client.pem",
                 "--tlsCertificateKeyFilePassword",
                 "qwerty");
// TODO: SERVER-80502 Enable the commented-out tests once the shell actually uses GRPCTransportLayer
// to connect.

// testShellConnect(false,
//                  `mongodb://${host}/?gRPC=true`,
//                  "--tls",
//                  "--tlsCAFile",
//                  tlsCAFile,
//                  "--tlsCertificateKeyFile",
//                  "jstests/libs/client.pem",
//                  "--tlsCertificateKeyFilePassword",
//                  "qwerty");
testShellConnect(false,
                 `mongodb://${host}`,
                 "--gRPC",
                 "--tls",
                 "--tlsCAFile",
                 tlsCAFile,
                 "--tlsCRLFile",
                 "jstests/libs/crl.pem");
// testShellConnect(false,
//                  `mongodb://${host}/?gRPC=true`,
//                  "--tls",
//                  "--tlsCAFile",
//                  tlsCAFile,
//                  "--tlsCRLFile",
//                  "jstests/libs/crl.pem");
testShellConnect(
    false, `mongodb://${host}`, "--gRPC", "--tls", "--tlsCAFile", tlsCAFile, "--tlsFIPSMode");
assertConnectFailsWithErrorCode(`mongodb://user:password@${host}/?gRPC=true&tls=true`,
                                ErrorCodes.InvalidOptions);
assertConnectFailsWithErrorCode(`mongodb://${host}/?gRPC=true&tls=true&replicaSet=blah`,
                                ErrorCodes.InvalidOptions);

MongoRunner.stopMongod(mongod);
