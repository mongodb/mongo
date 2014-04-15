var baseName = "jstests_disk_datafile_options";

load('jstests/libs/command_line/test_parsed_options.js');

jsTest.log("Testing \"noprealloc\" command line option");
var expectedResult = {
    "parsed" : {
        "storage" : {
            "preallocDataFiles" : false
        }
    }
};
testGetCmdLineOptsMongod({ noprealloc : "" }, expectedResult);

jsTest.log("Testing \"storage.preallocDataFiles\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_prealloc.json",
        "storage" : {
            "preallocDataFiles" : true
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_prealloc.json" },
                         expectedResult);

jsTest.log("Testing with no explicit data file option setting");
expectedResult = {
    "parsed" : {
        "storage" : { }
    }
};
testGetCmdLineOptsMongod({}, expectedResult);

print(baseName + " succeeded.");
