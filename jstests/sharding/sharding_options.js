// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

import {
    testGetCmdLineOptsMongod,
    testGetCmdLineOptsMongodFailed
} from "jstests/libs/command_line/test_parsed_options.js";

//////////////////////////////
// Sharding role

// Command line options

jsTest.log("Testing \"--configsvr\" command line option");
let expectedResult = {
    "parsed": {"sharding": {"clusterRole": "configsvr"}, "replication": {"replSet": "dummy"}}
};
testGetCmdLineOptsMongod({configsvr: "", replSet: "dummy"}, expectedResult);

jsTest.log("Testing \"--shardsvr\" command line option");
expectedResult = {
    "parsed": {"sharding": {"clusterRole": "shardsvr"}, "replication": {"replSet": "dummy"}}
};
testGetCmdLineOptsMongod({shardsvr: "", replSet: "dummy"}, expectedResult);

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    jsTest.log("Ensure starting a standalone with \"--shardsvr\" fails");
    testGetCmdLineOptsMongodFailed({shardsvr: ""});

    jsTest.log("Ensure starting a standalone with \"--configsvr\" fails");
    testGetCmdLineOptsMongodFailed({configsvr: ""});
}

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
// Embedded router

// Command line options

jsTest.log("Testing \"--configsvr\" and \"--routerPort\" command line options");
expectedResult = {
    "parsed": {
        "sharding": {"clusterRole": "configsvr"},
        "replication": {"replSet": "dummy"},
        "net": {"routerPort": 27016}
    }
};
testGetCmdLineOptsMongod({configsvr: "", routerPort: "", replSet: "dummy"}, expectedResult);
expectedResult = {
    "parsed": {
        "sharding": {"clusterRole": "configsvr"},
        "replication": {"replSet": "dummy"},
        "net": {"routerPort": 25000}
    }
};
testGetCmdLineOptsMongod({configsvr: "", routerPort: "25000", replSet: "dummy"}, expectedResult);

jsTest.log("Testing \"--shardsvr\" and \"--routerPort\" command line options");
expectedResult = {
    "parsed": {
        "sharding": {"clusterRole": "shardsvr"},
        "replication": {"replSet": "dummy"},
        "net": {"routerPort": 27016}
    }
};
testGetCmdLineOptsMongod({shardsvr: "", routerPort: "", replSet: "dummy"}, expectedResult);
expectedResult = {
    "parsed": {
        "sharding": {"clusterRole": "shardsvr"},
        "replication": {"replSet": "dummy"},
        "net": {"routerPort": 25000}
    }
};
testGetCmdLineOptsMongod({shardsvr: "", routerPort: "25000", replSet: "dummy"}, expectedResult);

jsTest.log("Ensure starting a replica set with \"--routerPort\" fails");
testGetCmdLineOptsMongodFailed({routerPort: "", replSet: "dummy"});

jsTest.log("Ensure starting a standalone with \"--routerPort\" fails");
testGetCmdLineOptsMongodFailed({routerPort: ""});

// Configuration file options

jsTest.log(
    "Testing \"sharding.clusterRole = configsvr\" and \"net.routerPort = 27016\" config file options");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_router_with_config_role.json",
        "sharding": {"clusterRole": "configsvr"},
        "replication": {"replSetName": "dummy"},
        "net": {"routerPort": 27016}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_router_with_config_role.json"},
                         expectedResult);

jsTest.log(
    "Testing \"sharding.clusterRole = shardsvr\" and \"net.routerPort = 27016\" config file options");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/enable_router_with_shard_role.json",
        "sharding": {"clusterRole": "shardsvr"},
        "replication": {"replSetName": "dummy"},
        "net": {"routerPort": 27016}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/enable_router_with_shard_role.json"},
                         expectedResult);

jsTest.log("Ensure starting a replica set with \"net.routerPort = 27016\" fails");
testGetCmdLineOptsMongodFailed(
    {config: "jstests/libs/config_files/enable_router_with_replicaset.json"});

jsTest.log("Ensure starting a standalone with \"net.routerPort = 27016\" fails");
testGetCmdLineOptsMongodFailed(
    {config: "jstests/libs/config_files/enable_router_with_standalone.json"});
