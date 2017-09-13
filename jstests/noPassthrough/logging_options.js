var baseName = "jstests_core_logging_options";

load('jstests/libs/command_line/test_parsed_options.js');

// Verbosity testing
jsTest.log("Testing \"verbose\" command line option with no args");
var expectedResult = {"parsed": {"systemLog": {"verbosity": 1}}};
testGetCmdLineOptsMongod({verbose: ""}, expectedResult);

jsTest.log("Testing \"verbose\" command line option with one \"v\"");
var expectedResult = {"parsed": {"systemLog": {"verbosity": 1}}};
testGetCmdLineOptsMongod({verbose: "v"}, expectedResult);

jsTest.log("Testing \"verbose\" command line option with two \"v\"s");
var expectedResult = {"parsed": {"systemLog": {"verbosity": 2}}};
testGetCmdLineOptsMongod({verbose: "vv"}, expectedResult);

jsTest.log("Testing \"v\" command line option");
var expectedResult = {"parsed": {"systemLog": {"verbosity": 1}}};
// Currently the test converts "{ v : 1 }" to "-v" when it spawns the binary.
testGetCmdLineOptsMongod({v: 1}, expectedResult);

jsTest.log("Testing \"vv\" command line option");
var expectedResult = {"parsed": {"systemLog": {"verbosity": 2}}};
// Currently the test converts "{ v : 2 }" to "-vv" when it spawns the binary.
testGetCmdLineOptsMongod({v: 2}, expectedResult);

jsTest.log("Testing \"systemLog.verbosity\" config file option");
expectedResult = {
    "parsed":
        {"config": "jstests/libs/config_files/set_verbosity.json", "systemLog": {"verbosity": 5}}
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_verbosity.json"}, expectedResult);

// log component verbosity
jsTest.log("Testing \"systemLog.component.verbosity\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_component_verbosity.json",
        "systemLog": {
            "verbosity": 2,
            "component": {
                "accessControl": {"verbosity": 0},
                "storage": {"verbosity": 3, "journal": {"verbosity": 5}}
            }
        }
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_component_verbosity.json"},
                         expectedResult);

// Log output testing
var baseDir = MongoRunner.dataPath + baseName;
var logDir = MongoRunner.dataPath + baseName + "/logs/";

// ensure log directory exists
assert(mkdir(baseDir));
assert(mkdir(logDir));

jsTest.log("Testing \"logpath\" command line option");
var expectedResult = {
    "parsed": {"systemLog": {"destination": "file", "path": logDir + "/mylog.log"}}
};
testGetCmdLineOptsMongod({logpath: logDir + "/mylog.log"}, expectedResult);

jsTest.log("Testing with no explicit logging setting");
expectedResult = {
    "parsed": {}
};
testGetCmdLineOptsMongod({}, expectedResult);

resetDbpath(baseDir);

print(baseName + " succeeded.");
