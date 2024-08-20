// Test --host with a replica set.
// @tags: [requires_replication]
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (_isWindows()) {
    // Windows is prone to slow DNS resolution issues, which causes this test to fail
    // even if the shell should eventually connect to the server. Since we are testing
    // valid/invalid legacy shell options, it should be sufficient to test on other
    // variants and reduce noise on Windows.
    jsTest.log("Skipping test on Windows");
    quit();
}

const replSetName = 'hostTestReplSetName';

// This "inner_mode" method of spawning a replset and re-running was copied from
// host_connection_string_validation.js
if ("undefined" == typeof inner_mode) {
    jsTest.log("Outer mode test starting a replica set");

    const replTest = new ReplSetTest({name: replSetName, nodes: 2});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();

    const args = [
        "mongo",
        "--nodb",
        "--eval",
        "inner_mode=true;port=" + primary.port + ";",
        "jstests/noPassthroughWithMongod/replset_host_connection_validation.js"
    ];
    const exitCode = _runMongoProgram(...args);
    jsTest.log("Inner mode test finished, exit code was " + exitCode);

    replTest.stopSet();
    // Pass the inner test's exit code back as the outer test's exit code
    if (exitCode != 0) {
        doassert("inner test failed with exit code " + exitCode);
    }
    quit();
}

function testHost(host, uri, ok) {
    const exitCode = runMongoProgram('mongo', '--eval', ';', '--host', host, uri);
    if (ok) {
        assert.eq(exitCode, 0, "failed to connect with `--host " + host + "`");
    } else {
        assert.neq(exitCode, 0, "unexpectedly succeeded to connect with `--host " + host + "`");
    }
}

function runConnectionStringTestFor(connectionString, uri, ok) {
    print("* Testing: --host " + connectionString + " " + uri);
    if (!ok) {
        print("  This should fail");
    }
    testHost(connectionString, uri, ok);
}

function expSuccess(str) {
    runConnectionStringTestFor(str, '', true);
    if (!str.startsWith('mongodb://')) {
        runConnectionStringTestFor(str, 'dbname', true);
    }
}

function expFailure(str) {
    runConnectionStringTestFor(str, '', false);
}

expSuccess(`localhost:${port}`);
expSuccess(`${replSetName}/localhost:${port}`);
expSuccess(`${replSetName}/localhost:${port},[::1]:${port}`);
expSuccess(`${replSetName}/localhost:${port},`);
expSuccess(`${replSetName}/localhost:${port},,`);
expSuccess(`mongodb://localhost:${port}/admin?replicaSet=${replSetName}`);
expSuccess(`mongodb://localhost:${port}`);

expFailure(',');
expFailure(',,');
expFailure(`${replSetName}/`);
expFailure(`${replSetName}/,`);
expFailure(`${replSetName}/,,`);
expFailure(`${replSetName}//not/a/socket`);
expFailure(`mongodb://localhost:${port}/admin?replicaSet=`);
expFailure('mongodb://localhost:');
expFailure(`mongodb://:${port}`);

jsTest.log("SUCCESSFUL test completion");
