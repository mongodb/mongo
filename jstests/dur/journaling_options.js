var baseName = "jstests_dur_journaling_options";

load('jstests/libs/command_line/test_parsed_options.js');

jsTest.log("Testing \"dur\" command line option");
var expectedResult = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOptsMongod({ dur : "" }, expectedResult);

jsTest.log("Testing \"nodur\" command line option");
expectedResult = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : false
            }
        }
    }
};
testGetCmdLineOptsMongod({ nodur : "" }, expectedResult);

jsTest.log("Testing \"journal\" command line option");
expectedResult = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOptsMongod({ journal : "" }, expectedResult);

jsTest.log("Testing \"nojournal\" command line option");
expectedResult = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : false
            }
        }
    }
};
testGetCmdLineOptsMongod({ nojournal : "" }, expectedResult);

jsTest.log("Testing \"storage.journal.enabled\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_journal.json",
        "storage" : {
            "journal" : {
                "enabled" : false
            }
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_journal.json" },
                         expectedResult);

jsTest.log("Testing with no explicit journal setting");
expectedResult = {
    "parsed" : {
        "storage" : { }
    }
};
testGetCmdLineOptsMongod({}, expectedResult);

print(baseName + " succeeded.");
