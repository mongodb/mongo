// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

import {
    testGetCmdLineOptsMongod,
    testGetCmdLineOptsMongodFailed,
} from "jstests/libs/command_line/test_parsed_options.js";

//////////////////////////////
// Sharding role

// Command line options

jsTest.log('Testing "--configsvr" command line option');
let expectedResult = {
    "parsed": {"sharding": {"clusterRole": "configsvr"}, "replication": {"replSet": "dummy"}},
};
testGetCmdLineOptsMongod({configsvr: "", replSet: "dummy"}, expectedResult);

jsTest.log('Testing "--shardsvr" command line option');
expectedResult = {
    "parsed": {"sharding": {"clusterRole": "shardsvr"}, "replication": {"replSet": "dummy"}},
};
testGetCmdLineOptsMongod({shardsvr: "", replSet: "dummy"}, expectedResult);

if (!jsTestOptions().useAutoBootstrapProcedure) {
    // TODO: SERVER-80318 Remove block
    jsTest.log('Ensure starting a standalone with "--shardsvr" fails');
    testGetCmdLineOptsMongodFailed({shardsvr: ""});

    jsTest.log('Ensure starting a standalone with "--configsvr" fails');
    testGetCmdLineOptsMongodFailed({configsvr: ""});
}

// Configuration file options

jsTest.log('Testing "sharding.clusterRole = shardsvr" config file option');
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_shardingrole_shardsvr.json",
        "sharding": {"clusterRole": "shardsvr"},
        "replication": {"replSetName": "dummy"},
    },
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_shardingrole_shardsvr.json"}, expectedResult);

jsTest.log('Testing "sharding.clusterRole = configsvr" config file option');
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_shardingrole_configsvr.json",
        "sharding": {"clusterRole": "configsvr"},
        "replication": {"replSetName": "dummy"},
    },
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_shardingrole_configsvr.json"}, expectedResult);
