var baseName = "jstests_sharding_sharding_options";

load('jstests/libs/command_line/test_parsed_options.js');



// Move Paranoia
jsTest.log("Testing \"moveParanoia\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "archiveMovedChunks" : true
        }
    }
};
testGetCmdLineOptsMongod({ moveParanoia : "" }, expectedResult);

jsTest.log("Testing \"noMoveParanoia\" command line option");
expectedResult = {
    "parsed" : {
        "sharding" : {
            "archiveMovedChunks" : false
        }
    }
};
testGetCmdLineOptsMongod({ noMoveParanoia : "" }, expectedResult);

jsTest.log("Testing \"sharding.archiveMovedChunks\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_paranoia.json",
        "sharding" : {
            "archiveMovedChunks" : true
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_paranoia.json" },
                         expectedResult);



// Sharding Role
jsTest.log("Testing \"configsvr\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "clusterRole" : "configsvr"
        }
    }
};
testGetCmdLineOptsMongod({ configsvr : "" }, expectedResult);

jsTest.log("Testing \"shardsvr\" command line option");
expectedResult = {
    "parsed" : {
        "sharding" : {
            "clusterRole" : "shardsvr"
        }
    }
};
testGetCmdLineOptsMongod({ shardsvr : "" }, expectedResult);

jsTest.log("Testing \"sharding.clusterRole\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_shardingrole.json",
        "sharding" : {
            "clusterRole" : "configsvr"
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/set_shardingrole.json" },
                         expectedResult);



// Auto Splitting
jsTest.log("Testing \"noAutoSplit\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "autoSplit" : false
        }
    }
};
testGetCmdLineOptsMongos({ noAutoSplit : "" }, expectedResult);

jsTest.log("Testing \"sharding.autoSplit\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_autosplit.json",
        "sharding" : {
            "autoSplit" : true
        }
    }
};
testGetCmdLineOptsMongos({ config : "jstests/libs/config_files/enable_autosplit.json" },
                         expectedResult);

print(baseName + " succeeded.");
