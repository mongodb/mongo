var baseName = "jstests_core_logging_options";

function removeOptionsAddedByFramework(getCmdLineOptsResult) {
    // Remove options that we are not interested in checking, but that get set by the test
    delete getCmdLineOptsResult.parsed.setParameter
    delete getCmdLineOptsResult.parsed.storage
    delete getCmdLineOptsResult.parsed.net
    delete getCmdLineOptsResult.parsed.fastsync
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

jsTest.log("Testing \"verbose\" command line option with no args");
var expectedResult = {
    "parsed" : {
        "systemLog" : {
            "verbosity" : 1
        }
    }
};
testGetCmdLineOpts({ verbose : "" }, expectedResult);

jsTest.log("Testing \"verbose\" command line option with one \"v\"");
var expectedResult = {
    "parsed" : {
        "systemLog" : {
            "verbosity" : 1
        }
    }
};
testGetCmdLineOpts({ verbose : "v" }, expectedResult);

jsTest.log("Testing \"verbose\" command line option with two \"v\"s");
var expectedResult = {
    "parsed" : {
        "systemLog" : {
            "verbosity" : 2
        }
    }
};
testGetCmdLineOpts({ verbose : "vv" }, expectedResult);

jsTest.log("Testing \"v\" command line option");
var expectedResult = {
    "parsed" : {
        "systemLog" : {
            "verbosity" : 1
        }
    }
};
// Currently the test converts "{ v : 1 }" to "-v" when it spawns the binary.
testGetCmdLineOpts({ v : 1 }, expectedResult);

jsTest.log("Testing \"vv\" command line option");
var expectedResult = {
    "parsed" : {
        "systemLog" : {
            "verbosity" : 2
        }
    }
};
// Currently the test converts "{ v : 2 }" to "-vv" when it spawns the binary.
testGetCmdLineOpts({ v : 2 }, expectedResult);

jsTest.log("Testing \"systemLog.verbosity\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_verbosity.json",
        "systemLog" : {
            "verbosity" : 5
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/set_verbosity.json" }, expectedResult);

jsTest.log("Testing with no explicit verbosity setting");
expectedResult = {
    "parsed" : { }
};
testGetCmdLineOpts({}, expectedResult);

print(baseName + " succeeded.");
