var baseName = "jstests_disk_datafile_options";

load('jstests/libs/command_line/test_parsed_options.js');

jsTest.log("Testing \"noprealloc\" command line option");
var expectedResult = {"parsed": {"storage": {"mmapv1": {"preallocDataFiles": false}}}};
testGetCmdLineOptsMongod({noprealloc: ""}, expectedResult);

jsTest.log("Testing \"storage.mmapv1.preallocDataFiles\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_prealloc.json",
        "storage": {"mmapv1": {"preallocDataFiles": true}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_prealloc.json"},
                         expectedResult);

jsTest.log("Testing with no explicit data file option setting");
expectedResult = {
    "parsed": {"storage": {}}
};
testGetCmdLineOptsMongod({}, expectedResult);

// Test that we preserve switches explicitly set to false in config files.  See SERVER-13439.
jsTest.log("Testing explicitly disabled \"noprealloc\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_noprealloc.ini",
        "storage": {"mmapv1": {"preallocDataFiles": true}}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_noprealloc.ini"},
                         expectedResult);

print(baseName + " succeeded.");
