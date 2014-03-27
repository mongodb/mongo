var baseName = "jstests_core_network_options";

function removeOptionsAddedByFramework(getCmdLineOptsResult) {
    // Remove options that we are not interested in checking, but that get set by the test
    delete getCmdLineOptsResult.parsed.setParameter
    delete getCmdLineOptsResult.parsed.storage
    delete getCmdLineOptsResult.parsed.net.port
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

jsTest.log("Testing \"objcheck\" command line option");
var expectedResult = {
    "parsed" : {
        "net" : {
            "wireObjectCheck" : true
        }
    }
};
testGetCmdLineOpts({ objcheck : "" }, expectedResult);

jsTest.log("Testing \"noobjcheck\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "wireObjectCheck" : false
        }
    }
};
testGetCmdLineOpts({ noobjcheck : "" }, expectedResult);

jsTest.log("Testing \"net.wireObjectCheck\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_objcheck.json",
        "net" : {
            "wireObjectCheck" : true
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/enable_objcheck.json" }, expectedResult);

jsTest.log("Testing with no explicit object check setting");
expectedResult = {
    "parsed" : {
        "net" : { }
    }
};
testGetCmdLineOpts({}, expectedResult);

print(baseName + " succeeded.");
