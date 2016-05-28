var baseName = "jstests_repl_repl_options";

load('jstests/libs/command_line/test_parsed_options.js');

jsTest.log("Testing \"replSet\" command line option");
var expectedResult = {"parsed": {"replication": {"replSet": "mycmdlinename"}}};
testGetCmdLineOptsMongod({replSet: "mycmdlinename"}, expectedResult);

jsTest.log("Testing \"replication.replSetName\" config file option");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_replsetname.json",
        "replication": {"replSetName": "myconfigname"}
    }
};
testGetCmdLineOptsMongod({config: "jstests/libs/config_files/set_replsetname.json"},
                         expectedResult);

jsTest.log("Testing override of \"replication.replSetName\" config file option with \"replSet\"");
expectedResult = {
    "parsed": {
        "config": "jstests/libs/config_files/set_replsetname.json",
        "replication": {"replSet": "mycmdlinename"}
    }
};
testGetCmdLineOptsMongod(
    {config: "jstests/libs/config_files/set_replsetname.json", replSet: "mycmdlinename"},
    expectedResult);

print(baseName + " succeeded.");
