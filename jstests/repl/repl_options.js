var baseName = "jstests_repl_repl_options";

function removeOptionsAddedByFramework(getCmdLineOptsResult) {
    // Remove options that we are not interested in checking, but that get set by the test
    delete getCmdLineOptsResult.parsed.setParameter
    delete getCmdLineOptsResult.parsed.storage
    delete getCmdLineOptsResult.parsed.net
    delete getCmdLineOptsResult.parsed.fastsync
    delete getCmdLineOptsResult.parsed.security
    return getCmdLineOptsResult;
}

function testGetCmdLineOpts(mongoRunnerConfig, expectedResult) {

    // Start mongod with options
    var mongod = MongoRunner.runMongod(mongoRunnerConfig);

    // Get the parsed options
    var getCmdLineOptsResult = mongod.adminCommand("getCmdLineOpts");
    printjson(getCmdLineOptsResult);

    // Remove options added by the test framework
    getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);

    // Make sure the options are equal to what we expect
    assert.docEq(getCmdLineOptsResult.parsed, expectedResult.parsed);

    // Cleanup
    MongoRunner.stopMongod(mongod.port);
}

jsTest.log("Testing \"replSet\" command line option");
var expectedResult = {
    "parsed" : {
        "replication" : {
            "replSet" : "mycmdlinename"
        }
    }
};
testGetCmdLineOpts({ replSet : "mycmdlinename" }, expectedResult);

jsTest.log("Testing \"replication.replSetName\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_replsetname.json",
        "replication" : {
            "replSetName" : "myconfigname"
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/set_replsetname.json" }, expectedResult);

jsTest.log("Testing override of \"replication.replSetName\" config file option with \"replSet\"");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_replsetname.json",
        "replication" : {
            "replSet" : "mycmdlinename"
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/set_replsetname.json",
                     replSet : "mycmdlinename" }, expectedResult);



print(baseName + " succeeded.");
