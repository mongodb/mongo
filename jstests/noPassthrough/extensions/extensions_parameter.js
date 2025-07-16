/**
 * Tests the --extensions startup parameter on mongos and mongod.
 * This is NOT robustly testing loading extensions but rather ensuring that the extensions parameter
 * provided to the server is properly passed to the extension loading module.
 * @tags: [featureFlagExtensionsApi]
 */

import {isLinux} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const pathToExtensionFoo = MongoRunner.getInstallPath("..", "lib", "libfoo_extension.so");

// Create a ShardingTest so that we have a config DB for mongos to point to in our test. We don't
// use ShardingTest directly because repeated failed ShardingTest startups causes issues in the test
// environment. This also reduces the amount of times we have to start a whole sharded cluster in
// the test.
const st = new ShardingTest({shards: 1, mongos: 1});

function runTest({options, shouldFail = false, validateExitCode = true}) {
    function tryStartup(startupFn) {
        try {
            startupFn();
            assert(!shouldFail, "Expected startup to fail but it succeeded");
        } catch (e) {
            assert(shouldFail, `Expected startup to succeed but it failed: ${e}`);
            assert(e instanceof MongoRunner.StopError, e);
            if (validateExitCode) {
                assert.eq(e.returnCode, MongoRunner.EXIT_BADOPTIONS, e);
            }
        }
    }
    // Test mongod.
    tryStartup(() => {
        const conn = MongoRunner.runMongod(options);
        MongoRunner.stopMongod(conn);
    });
    // Test mongos.
    tryStartup(() => {
        const conn =
            MongoRunner.runMongos(Object.assign({configdb: st.configRS.getURL()}, options));
        MongoRunner.stopMongos(conn);
    });
}

if (isLinux()) {
    // Empty filename (equivalent to no argument being provided to the parameter, so failure happens
    // at a higher level when parsing parameters).
    runTest({options: {extensions: ""}, shouldFail: true, validateExitCode: false});
    // Extensions parameter is not allowed when the feature flag is disabled.
    runTest({
        options: {extensions: pathToExtensionFoo, featureFlagExtensionsAPI: false},
        shouldFail: true
    });
    // Extensions is a scalar, non-string.
    runTest({options: {extensions: 12345}, shouldFail: true});
    // Path to extension does not exist.
    runTest({options: {extensions: "path/does/not/exist.so"}, shouldFail: true});
    // TODO SERVER-106929 Enable these tests.
    // Single valid extension.
    // runTest({options: {extensions: pathToExtensionFoo}});
    // List of valid extensions.
    // runTest({options: {extensions: [pathToExtensionFoo, pathToExtensionFoo]}});
} else {
    // Startup should fail because we are attempting to load an extension on a platform that is not
    // linux.
    runTest({options: {extensions: pathToExtensionFoo}, shouldFail: true});
}

st.stop();
