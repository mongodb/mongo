var baseName = "jstests_core_profile_options";

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

jsTest.log("Testing \"profile\" command line option with profiling off");
var expectedResult = {
    "parsed" : {
        "operationProfiling" : {
            "mode" : "off"
        }
    }
};
testGetCmdLineOpts({ profile : "0" }, expectedResult);

jsTest.log("Testing \"profile\" command line option with profiling slow operations on");
var expectedResult = {
    "parsed" : {
        "operationProfiling" : {
            "mode" : "slowOp"
        }
    }
};
testGetCmdLineOpts({ profile : "1" }, expectedResult);

jsTest.log("Testing \"profile\" command line option with profiling all on");
var expectedResult = {
    "parsed" : {
        "operationProfiling" : {
            "mode" : "all"
        }
    }
};
testGetCmdLineOpts({ profile : "2" }, expectedResult);

jsTest.log("Testing \"operationProfiling.mode\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_profiling.json",
        "operationProfiling" : {
            "mode" : "all"
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/set_profiling.json" }, expectedResult);



print(baseName + " succeeded.");
