/**
 * Tests that the 'processManagement.loadExtensions' field is set in the parsed options correctly,
 * using both the startup parameter and the config file.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {
    testGetCmdLineOptsMongod,
    testGetCmdLineOptsMongos,
    writeJSONConfigFile,
} from "jstests/libs/command_line/test_parsed_options.js";
import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    checkPlatformCompatibleWithExtensions,
    getExtensionConfDir,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

const extensionNames = generateExtensionConfigs([
    "libfoo_mongo_extension.so",
    "libbar_mongo_extension.so",
]);
const confDir = getExtensionConfDir();

try {
    // Test loading a single extension in mongod and mongos.
    let expectedResult = {
        "parsed": {
            "processManagement": {
                "extensionsConfigPath": confDir,
                "loadExtensions": [extensionNames[0]],
            },
        },
    };
    testGetCmdLineOptsMongod(
        {loadExtensions: extensionNames[0], extensionsConfigPath: confDir},
        expectedResult,
    );
    testGetCmdLineOptsMongos(
        {loadExtensions: extensionNames[0], extensionsConfigPath: confDir},
        expectedResult,
    );

    // Test loading multiple extensions in mongod and mongos.
    // NOTE: The shell helper passes the commandline string array as an array with a single string (all
    // extensions concatenated with commas), so that is the expected result. The options are parsed
    // correctly internally, and this is tested more in extensions_parameter.js.
    expectedResult = {
        "parsed": {
            "processManagement": {
                "extensionsConfigPath": confDir,
                "loadExtensions": [extensionNames[0] + "," + extensionNames[1]],
            },
        },
    };
    // Test passing multiple extensions as array.
    testGetCmdLineOptsMongod(
        {loadExtensions: [extensionNames[0], extensionNames[1]], extensionsConfigPath: confDir},
        expectedResult,
    );
    testGetCmdLineOptsMongos(
        {loadExtensions: [extensionNames[0], extensionNames[1]], extensionsConfigPath: confDir},
        expectedResult,
    );

    // Test loading via config file.
    const extensionFooConfig = writeJSONConfigFile("enables_single_extension", {
        processManagement: {loadExtensions: [extensionNames[0]]},
    });
    expectedResult = {
        "parsed": {
            "config": extensionFooConfig,
            "processManagement": {
                "extensionsConfigPath": confDir,
                "loadExtensions": [extensionNames[0]],
            },
        },
    };
    testGetCmdLineOptsMongod(
        {config: extensionFooConfig, extensionsConfigPath: confDir},
        expectedResult,
    );
    testGetCmdLineOptsMongos(
        {config: extensionFooConfig, extensionsConfigPath: confDir},
        expectedResult,
    );

    const extensionsFooAndBarConfig = writeJSONConfigFile("enables_multiple_extensions", {
        processManagement: {loadExtensions: [extensionNames[0], extensionNames[1]]},
    });
    expectedResult = {
        "parsed": {
            "config": extensionsFooAndBarConfig,
            "processManagement": {
                "extensionsConfigPath": confDir,
                "loadExtensions": [extensionNames[0], extensionNames[1]],
            },
        },
    };
    testGetCmdLineOptsMongod(
        {config: extensionsFooAndBarConfig, extensionsConfigPath: confDir},
        expectedResult,
    );
    testGetCmdLineOptsMongos(
        {config: extensionsFooAndBarConfig, extensionsConfigPath: confDir},
        expectedResult,
    );

    // Test loading via config file and parameter at the same time.
    expectedResult = {
        "parsed": {
            "config": extensionFooConfig,
            "processManagement": {
                "extensionsConfigPath": confDir,
                "loadExtensions": [extensionNames[0], extensionNames[1]],
            },
        },
    };
    testGetCmdLineOptsMongod(
        {
            config: extensionFooConfig,
            loadExtensions: extensionNames[1],
            extensionsConfigPath: confDir,
        },
        expectedResult,
    );
    testGetCmdLineOptsMongos(
        {
            config: extensionFooConfig,
            loadExtensions: extensionNames[1],
            extensionsConfigPath: confDir,
        },
        expectedResult,
    );
} finally {
    deleteExtensionConfigs(extensionNames);
}
