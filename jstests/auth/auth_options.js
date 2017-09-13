var baseName = "jstests_auth_auth_options";

load('jstests/libs/command_line/test_parsed_options.js');

jsTest.log("Testing \"auth\" command line option");
var expectedResult = {"parsed": {"security": {"authorization": "enabled"}}};

testGetCmdLineOptsMongod({auth: ""}, expectedResult);

jsTest.log("Testing \"noauth\" command line option");
expectedResult = {
    "parsed": {"security": {"authorization": "disabled"}}
};
testGetCmdLineOptsMongod({noauth: ""}, expectedResult);

jsTest.log("Testing \"security.authorization\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_auth.json",
        "security": {"authorization": "enabled"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_auth.json"}, expectedResult);

jsTest.log("Testing with no explicit object check setting");
expectedResult = {
    "parsed": {}
};
testGetCmdLineOptsMongod({}, expectedResult);

// Test that we preserve switches explicitly set to false in config files.  See SERVER-13439.
jsTest.log("Testing explicitly disabled \"auth\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_auth.ini",
        "security": {"authorization": "disabled"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_auth.ini"}, expectedResult);

jsTest.log("Testing explicitly disabled \"noauth\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_noauth.ini",
        "security": {"authorization": "enabled"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_noauth.ini"}, expectedResult);

print(baseName + " succeeded.");
