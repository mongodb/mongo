import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// Constructs a new Mongo instance with the provided URI and asserts it fails with the provided
// error code.
function assertConnectFailsWithErrorCode(uri, errorCode) {
    jsTestLog(`Connecting to ${uri}`);
    assert.throwsWithCode(() => new Mongo(uri), errorCode);
}

// Runs a new shell process with the provided arguments and asserts that its exit code matches `ok`
// (true for success). This is used over assertConnectFailsWithErrorCode when CLI-only arguments
// need to be specified.
function testShellConnect(ok, ...args) {
    const cmd = 'assert.commandWorked(db.runCommand({hello: 1}));';
    const exitCode = runMongoProgram('mongo', '--eval', cmd, ...args);
    if (ok) {
        assert.eq(exitCode, 0, "failed to connect with `" + args.join(' ') + "`");
    } else {
        assert.neq(exitCode, 0, "unexpectedly succeeded connecting with `" + args.join(' ') + "`");
    }
}

const mongod = MongoRunner.runMongod({});

if (!FeatureFlagUtil.isPresentAndEnabled(mongod.getDB("admin"), "GRPC")) {
    jsTestLog("Skipping shell_grpc_uri.js test due to featureFlagGRPC being disabled");
    MongoRunner.stopMongod(mongod);
    quit();
}

const host = `localhost:${mongod.fullOptions.grpcPort}`;

function testGRPCConnect(ok, ...args) {
    testShellConnect(ok, `mongodb://${host}`, '--gRPC', ...args);
    testShellConnect(ok, `mongodb://${host}/?gRPC=true`, ...args);
}

testGRPCConnect(true);

// Options currently prohibited when using gRPC.
testGRPCConnect(false, '--tlsCRLFile', 'jstests/libs/crl.pem');
testGRPCConnect(false,
                '--tlsCertificateKeyFile',
                'jstests/libs/password_protected.pem',
                '--tlsCertificateKeyFilePassword',
                'qwerty');
testGRPCConnect(false, '--tlsFIPSMode');

assertConnectFailsWithErrorCode(`mongodb://user:password@${host}/?gRPC=true&tls=true`,
                                ErrorCodes.InvalidOptions);
assertConnectFailsWithErrorCode(`mongodb://${host}/?gRPC=true&tls=true&replicaSet=blah`,
                                ErrorCodes.InvalidOptions);

MongoRunner.stopMongod(mongod);
