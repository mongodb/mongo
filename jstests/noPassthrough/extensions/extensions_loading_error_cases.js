/**
 * Tests error cases when using the --loadExtensions startup parameter on mongos and mongod.
 *
 * This includes testing cases where the host rejects the parsed options, file does not exist, and
 * two cases where the extension is rejected by the host during loading.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */
import {isLinux} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {generateExtensionConfigs, deleteExtensionConfigs} from "jstests/noPassthrough/libs/extension_helpers.js";

const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");
const pathToMissingSymbolExtension = MongoRunner.getExtensionPath("libno_symbol_bad_extension.so");
const pathToDuplicateStageExtension = MongoRunner.getExtensionPath("libduplicate_stage_descriptor_bad_extension.so");

const extensionNames = generateExtensionConfigs([
    pathToExtensionFoo,
    pathToMissingSymbolExtension,
    pathToDuplicateStageExtension,
]);

// Create a ShardingTest so that we have a config DB for mongos to point to in our test. We don't
// use ShardingTest directly because repeated failed ShardingTest startups causes issues in the test
// environment. This also reduces the amount of times we have to start a whole sharded cluster in
// the test.
const st = new ShardingTest({shards: 1, mongos: 1});

function runTest({options, validateExitCode = true}) {
    function tryStartup(startupFn) {
        try {
            startupFn();
            assert(false, "Expected startup to fail but it succeeded");
        } catch (e) {
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
        const conn = MongoRunner.runMongos(Object.assign({configdb: st.configRS.getURL()}, options));
        MongoRunner.stopMongos(conn);
    });
}

try {
    if (isLinux()) {
        // Empty filename (equivalent to no argument being provided to the parameter, so failure happens
        // at a higher level when parsing parameters).
        runTest({options: {loadExtensions: ""}, validateExitCode: false});
        // Extensions parameter is not allowed when the feature flag is disabled.
        runTest({
            options: {loadExtensions: extensionNames[0], setParameter: {featureFlagExtensionsAPI: false}},
        });
        // Extensions is a scalar, non-string.
        runTest({options: {loadExtensions: 12345}});
        // Path to extension does not exist.
        runTest({options: {loadExtensions: "path/does/not/exist.so"}});
        // Path to extension with an .so that is missing the get_mongodb_extension symbol.
        runTest({options: {loadExtensions: extensionNames[1]}});
        // Path to extension that attempts to register duplicate stage descriptors.
        runTest({options: {loadExtensions: extensionNames[2]}});
    } else {
        // Startup should fail because we are attempting to load an extension on a platform that is not
        // linux.
        runTest({options: {loadExtensions: extensionNames[0]}});
    }
} finally {
    st.stop();
    deleteExtensionConfigs(extensionNames);
}
