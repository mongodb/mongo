// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

var baseName = "jstests_sharding_sharding_options";

load('jstests/libs/command_line/test_parsed_options.js');

//////////////////////////////
// Sharding role

// Command line options

jsTest.log("Testing \"--configsvr\" command line option");
var expectedResult = {
    "parsed": {"sharding": {"clusterRole": "configsvr"}, "replication": {"replSet": "dummy"}}
};
testGetCmdLineOptsMongod({configsvr: "", replSet: "dummy"}, expectedResult);

jsTest.log("Testing \"--shardsvr\" command line option");
expectedResult = {
    "parsed": {"sharding": {"clusterRole": "shardsvr"}, "replication": {"replSet": "dummy"}}
};
testGetCmdLineOptsMongod({shardsvr: "", replSet: "dummy"}, expectedResult);

jsTest.log("Ensure starting a standalone with \"--shardsvr\" fails");
testGetCmdLineOptsMongodFailed({shardsvr: ""});

jsTest.log("Ensure starting a standalone with \"--configsvr\" fails");
testGetCmdLineOptsMongodFailed({configsvr: ""});

// Configuration file options

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

//////////////////////////////
// Built-in router

// Command line options

jsTest.log("Testing \"--configsvr\" and \"--router\" command line options");
var expectedResult = {
    "parsed": {
        "sharding": {"clusterRole": "configsvr", "routerEnabled": true},
        "replication": {"replSet": "dummy"}
    }
};
testGetCmdLineOptsMongod({configsvr: "", router: "", replSet: "dummy"}, expectedResult);

jsTest.log("Testing \"--shardsvr\" and \"--router\" command line options");
var expectedResult = {
    "parsed": {
        "sharding": {"clusterRole": "shardsvr", "routerEnabled": true},
        "replication": {"replSet": "dummy"}
    }
};
testGetCmdLineOptsMongod({shardsvr: "", router: "", replSet: "dummy"}, expectedResult);

jsTest.log("Ensure starting a replica set with \"--router\" fails");
testGetCmdLineOptsMongodFailed({router: "", replSet: "dummy"});

jsTest.log("Ensure starting a standalone with \"--router\" fails");
testGetCmdLineOptsMongodFailed({router: ""});

// Configuration file options

jsTest.log(
    "Testing \"sharding.clusterRole = configsvr\" and \"sharding.routerEnabled = true\" config file options");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_router_with_config_role.json",
        "sharding": {"clusterRole": "configsvr", "routerEnabled": true},
        "replication": {"replSetName": "dummy"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_router_with_config_role.json"},
                         expectedResult);

jsTest.log(
    "Testing \"sharding.clusterRole = shardsvr\" and \"sharding.routerEnabled = true\" config file options");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_router_with_shard_role.json",
        "sharding": {"clusterRole": "shardsvr", "routerEnabled": true},
        "replication": {"replSetName": "dummy"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_router_with_shard_role.json"},
                         expectedResult);

jsTest.log("Ensure starting a replica set with \"sharding.routerEnabled = true\" fails");
testGetCmdLineOptsMongodFailed(
    {config: "jstests/libs/config_files/enable_router_with_replicaset.json"});

jsTest.log("Ensure starting a standalone with \"sharding.routerEnabled = true\" fails");
testGetCmdLineOptsMongodFailed(
    {config: "jstests/libs/config_files/enable_router_with_standalone.json"});
