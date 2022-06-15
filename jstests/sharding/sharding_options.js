// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

var baseName = "jstests_sharding_sharding_options";

load('jstests/libs/command_line/test_parsed_options.js');

// Move Paranoia
jsTest.log("Testing \"moveParanoia\" command line option");
var expectedResult = {"parsed": {"sharding": {"archiveMovedChunks": true}}};
testGetCmdLineOptsMongod({moveParanoia: ""}, expectedResult);

jsTest.log("Testing \"noMoveParanoia\" command line option");
expectedResult = {
    "parsed": {"sharding": {"archiveMovedChunks": false}}
};
testGetCmdLineOptsMongod({noMoveParanoia: ""}, expectedResult);

jsTest.log("Testing \"sharding.archiveMovedChunks\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_paranoia.json",
        "sharding": {"archiveMovedChunks": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_paranoia.json"},
                         expectedResult);

// Sharding Role
jsTest.log("Testing \"configsvr\" command line option");
var expectedResult = {
    "parsed": {"sharding": {"clusterRole": "configsvr"}, "replication": {"replSet": "dummy"}}
};
testGetCmdLineOptsMongod({configsvr: "", replSet: "dummy"}, expectedResult);

jsTest.log("Testing \"shardsvr\" command line option");
expectedResult = {
    "parsed": {"sharding": {"clusterRole": "shardsvr"}, "replication": {"replSet": "dummy"}}
};
testGetCmdLineOptsMongod({shardsvr: "", replSet: "dummy"}, expectedResult);

jsTest.log("Testing \"sharding.clusterRole = shardsvr\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_shardingrole_shardsvr.json",
        "sharding": {"clusterRole": "shardsvr"},
        "replication": {"replSetName": "dummy"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_shardingrole_shardsvr.json"},
                         expectedResult);

jsTest.log("Testing \"sharding.clusterRole = configsvr\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_shardingrole_configsvr.json",
        "sharding": {"clusterRole": "configsvr"},
        "replication": {"replSetName": "dummy"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_shardingrole_configsvr.json"},
                         expectedResult);

// Test that we preserve switches explicitly set to false in config files.  See SERVER-13439.
jsTest.log("Testing explicitly disabled \"moveParanoia\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_moveparanoia.ini",
        "sharding": {"archiveMovedChunks": false}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_moveparanoia.ini"},
                         expectedResult);

jsTest.log("Testing explicitly disabled \"noMoveParanoia\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/disable_nomoveparanoia.ini",
        "sharding": {"archiveMovedChunks": true}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/disable_nomoveparanoia.ini"},
                         expectedResult);

jsTest.log("Ensure starting a standalone with --shardsvr fails");
testGetCmdLineOptsMongodFailed({shardsvr: ""});

jsTest.log("Ensure starting a standalone with --configsvr fails");
testGetCmdLineOptsMongodFailed({configsvr: ""});

print(baseName + " succeeded.");
