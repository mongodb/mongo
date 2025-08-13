/**
 * Tests that the 'processManagement.loadExtensions' field is set in the parsed options correctly,
 * using both the startup parameter and the config file.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {
    testGetCmdLineOptsMongod,
    testGetCmdLineOptsMongos,
    writeJSONConfigFile
} from "jstests/libs/command_line/test_parsed_options.js";
import {isLinux} from "jstests/libs/os_helpers.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test on non-Linux platform");
    quit();
}

const pathToExtensionFoo = MongoRunner.getExtensionPath("libfoo_mongo_extension.so");
const pathToExtensionBar = MongoRunner.getExtensionPath("libbar_mongo_extension.so");

// Test loading a single extension in mongod and mongos.
let expectedResult = {"parsed": {"processManagement": {"loadExtensions": [pathToExtensionFoo]}}};
testGetCmdLineOptsMongod({loadExtensions: pathToExtensionFoo}, expectedResult);
testGetCmdLineOptsMongos({loadExtensions: pathToExtensionFoo}, expectedResult);

// Test loading multiple extensions in mongod and mongos.
// NOTE: The shell helper passes the commandline string array as an array with a single string (all
// extensions concatenated with commas), so that is the expected result. The options are parsed
// correctly internally, and this is tested more in extensions_parameter.js.
expectedResult = {
    "parsed":
        {"processManagement": {"loadExtensions": [pathToExtensionFoo + "," + pathToExtensionBar]}}
};
// Test passing multiple extensions as array.
testGetCmdLineOptsMongod({loadExtensions: [pathToExtensionFoo, pathToExtensionBar]},
                         expectedResult);
testGetCmdLineOptsMongos({loadExtensions: [pathToExtensionFoo, pathToExtensionBar]},
                         expectedResult);

// Test loading via config file.
const extensionFooConfig = writeJSONConfigFile(
    "enables_single_extension", {processManagement: {loadExtensions: [pathToExtensionFoo]}});
expectedResult = {
    "parsed": {
        "config": extensionFooConfig,
        "processManagement": {"loadExtensions": [pathToExtensionFoo]}
    }
};
testGetCmdLineOptsMongod({config: extensionFooConfig}, expectedResult);
testGetCmdLineOptsMongos({config: extensionFooConfig}, expectedResult);

const extensionsFooAndBarConfig = writeJSONConfigFile(
    "enables_multiple_extensions",
    {processManagement: {loadExtensions: [pathToExtensionFoo, pathToExtensionBar]}});
expectedResult = {
    "parsed": {
        "config": extensionsFooAndBarConfig,
        "processManagement": {"loadExtensions": [pathToExtensionFoo, pathToExtensionBar]}
    }
};
testGetCmdLineOptsMongod({config: extensionsFooAndBarConfig}, expectedResult);
testGetCmdLineOptsMongos({config: extensionsFooAndBarConfig}, expectedResult);

// Test loading via config file and parameter at the same time.
expectedResult = {
    "parsed": {
        "config": extensionFooConfig,
        "processManagement": {"loadExtensions": [pathToExtensionFoo, pathToExtensionBar]}
    }
};
testGetCmdLineOptsMongod({config: extensionFooConfig, loadExtensions: pathToExtensionBar},
                         expectedResult);
testGetCmdLineOptsMongos({config: extensionFooConfig, loadExtensions: pathToExtensionBar},
                         expectedResult);
