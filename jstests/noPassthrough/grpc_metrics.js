import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// For each shell invocation, we perform 4 + N operations.
// {hello:...}, {whatsmyuri:...}, {buildInfo:...}
// Then what ever operations are in the --eval body
// And finally an implicit {endSessions:...}
const kShellPrefixOperations = 3;
const kShellSuffixOperations = 1;

function evalCmd(uri, evalstr, ok = true, asyncCb = null) {
    const args = ['mongo', uri, '--eval', evalstr];

    let exitCode = undefined;
    if (asyncCb) {
        const pid = startMongoProgramNoConnect(...args);
        asyncCb(pid);
        exitCode = waitProgram(pid);
    } else {
        // Simple synchronous call.
        exitCode = runMongoProgram(...args);
    }

    assert(exitCode !== undefined, `"${evalstr}" did not run?`);
    const assertion = ok ? assert.eq : assert.neq;
    assertion(exitCode, 0, `While executing "${evalstr}"`);
}

function runCmd(uri, runOnDB, cmd, ok = true) {
    const evalFunc = function(dbname, cmd) {
        jsTest.log(assert.commandWorked(db.getSiblingDB(dbname).runCommand(cmd)));
    };
    const evalstr = `(${evalFunc})(${tojson(runOnDB)}, ${tojson(cmd)});`;
    evalCmd(uri, evalstr, ok);
}

function checkGRPCStats(conn, expect) {
    const grpcStats = assert.commandWorked(conn.adminCommand({serverStatus: 1})).gRPC;
    jsTest.log(grpcStats);

    function search(prefix, obj, expect) {
        return function(key) {
            assert(obj[key] !== undefined, `Missing '${prefix}.${key}' field`);
            if (typeof expect[key] == 'object') {
                assert.eq(typeof obj[key], 'object', `'${prefix}.${key}' expected object`);
                Object.keys(expect[key]).forEach(search(`${prefix}.${key}`, obj[key], expect[key]));
            } else {
                assert.eq(obj[key], expect[key], `'${prefix}.${key}' value mismatch`);
            }
        };
    }

    Object.keys(expect).forEach(search('serverStatus.gRPC', grpcStats, expect));
}

function runTest(conn) {
    let expect = {
        'streams': {
            'total': 0,
            'current': 0,
            'successful': 0,
        },
        'operations': {
            'total': 0,
            'active': 0,
        },
        'uniqueClientsSeen': 0,
    };

    // Test currently makes assumption that connections via Mongo objects are using ASIO
    // When that changes the count expectations will change as well.
    function expectSuccess(explicitOps = 1) {
        const implicitOps = kShellPrefixOperations + kShellSuffixOperations;
        expect.streams.total++;
        expect.streams.successful++;
        expect.operations.total += implicitOps + explicitOps;
        expect.uniqueClientsSeen++;
    }
    function expectFailed(opCountTotal) {
        expect.streams.total++;
        expect.operations.total += opCountTotal;
        expect.uniqueClientsSeen++;
    }
    function expectPartialSuccess(opCountTotal) {
        expectFailed(opCountTotal);
        expect.streams.successful++;
    }

    const uri = `mongodb://localhost:${conn.fullOptions.grpcPort}/?gRPC=true`;

    // Connect with {failureURI} to have the server abort the connection during the reply cycle.
    const failureURI = uri + '&appName=Failure%20Client';
    configureFailPoint(
        conn, 'sessionWorkflowDelayOrFailSendMessage', {appName: 'Failure Client'}, 'alwaysOn');

    runCmd(uri, 'admin', {ping: 1});
    expectSuccess();
    checkGRPCStats(conn, expect);

    runCmd(uri, 'admin', {noSuchCommand: 1}, false);
    // Althrough the execution of the command failed,
    // the stream itself, and the operation lifetime, succeeded.
    expectSuccess();
    checkGRPCStats(conn, expect);

    // The server fails to send its response via the stream, so it cancels the RPC,
    // thus not marking it as successful.
    runCmd(failureURI, 'admin', {ping: 1}, false);
    expectFailed(1);
    checkGRPCStats(conn, expect);

    // Killing the client while it's processing should cause a stream failure on the server.
    // While a test failure may take up to 15s, this test should take no longer than a normal shell
    // exection.
    const kSIGKILL = 9;
    const kShellShutdownDelay = 15 * 1000;
    const kShellStartTimeout = 5 * 1000;
    const kShellStartInterval = 500;
    clearRawMongoProgramOutput();
    evalCmd(uri, `print("Kill Test\\n"); sleep(${kShellShutdownDelay});`, false, function(pid) {
        // Wait for the output from the eval string so that we know prefix ops have completed,
        // then kill the shell so that the stream shuts down unsuccessfully.
        assert.soon(() => rawMongoProgramOutput().includes("Kill Test"),
                    "Shell start failure",
                    kShellStartTimeout,
                    kShellStartInterval);
        stopMongoProgramByPid(pid, kSIGKILL);
    });
    expectFailed(kShellPrefixOperations);
    checkGRPCStats(conn, expect);
}

const mongod = MongoRunner.runMongod({});

if (!FeatureFlagUtil.isPresentAndEnabled(mongod.getDB("admin"), "GRPC")) {
    jsTestLog("Skipping grpc_metrics.js test due to featureFlagGRPC being disabled");
    MongoRunner.stopMongod(mongod);
    quit();
}

runTest(mongod);
MongoRunner.stopMongod(mongod);
