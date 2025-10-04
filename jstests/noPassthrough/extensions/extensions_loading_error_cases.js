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
import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    checkExtensionFailsToLoad,
} from "jstests/noPassthrough/libs/extension_helpers.js";

const extensionNames = generateExtensionConfigs([
    "libfoo_mongo_extension.so",
    "libno_symbol_bad_extension.so",
    "libduplicate_stage_descriptor_bad_extension.so",
]);

// Create a ShardingTest so that we have a config DB for mongos to point to in our test. We don't
// use ShardingTest directly because repeated failed ShardingTest startups causes issues in the test
// environment. This also reduces the amount of times we have to start a whole sharded cluster in
// the test.
const st = new ShardingTest({shards: 1, mongos: 1});
try {
    if (isLinux()) {
        // Empty filename (equivalent to no argument being provided to the parameter, so failure happens
        // at a higher level when parsing parameters).
        checkExtensionFailsToLoad({options: {loadExtensions: ""}, st: st, validateExitCode: false});
        // Extensions parameter is not allowed when the feature flag is disabled.
        checkExtensionFailsToLoad({
            options: {loadExtensions: extensionNames[0], setParameter: {featureFlagExtensionsAPI: false}},
            st: st,
        });
        // Extensions is a scalar, non-string.
        checkExtensionFailsToLoad({options: {loadExtensions: 12345}, st: st});
        // Extension does not exist.
        checkExtensionFailsToLoad({options: {loadExtensions: "extension_does_not_exist"}, st: st});
        // Attempts to load a filepath.
        checkExtensionFailsToLoad({options: {loadExtensions: "etc/mongo/extensions/extension.conf"}, st: st});
        checkExtensionFailsToLoad({options: {loadExtensions: "path/to/extension_does_not_exist.so"}, st: st});
        // Extension with an .so that is missing the get_mongodb_extension symbol.
        checkExtensionFailsToLoad({options: {loadExtensions: extensionNames[1]}, st: st});
        // Extension that attempts to register duplicate stage descriptors.
        checkExtensionFailsToLoad({options: {loadExtensions: extensionNames[2]}, st: st});
    } else {
        // Startup should fail because we are attempting to load an extension on a platform that is not
        // linux.
        checkExtensionFailsToLoad({options: {loadExtensions: extensionNames[0]}, st: st});
    }
} finally {
    st.stop();
    deleteExtensionConfigs(extensionNames);
}
