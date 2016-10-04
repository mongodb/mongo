var baseName = "jstests_nopassthrough_javascript_options";

load('jstests/libs/command_line/test_parsed_options.js');

jsTest.log("Testing \"noscripting\" command line option");
var expectedResult = {"parsed": {"security": {"javascriptEnabled": false}}};
testGetCmdLineOptsMongod({noscripting: ""}, expectedResult);

jsTest.log("Testing explicitly disabled \"noscripting\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_noscripting.ini",
        "security": {"javascriptEnabled": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_noscripting.ini"},
                         expectedResult);

jsTest.log("Testing \"scriptingEnabled\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_scripting.json",
        "security": {"javascriptEnabled": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_scripting.json"},
                         expectedResult);

print(baseName + " succeeded.");
