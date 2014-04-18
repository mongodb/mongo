var baseName = "jstests_auth_auth_options";

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

jsTest.log("Testing \"auth\" command line option");
var expectedResult = {
    "parsed" : {
        "security" : {
            "authorization" : "enabled"
        }
    }
};
testGetCmdLineOpts({ auth : "" }, expectedResult);

jsTest.log("Testing \"noauth\" command line option");
expectedResult = {
    "parsed" : {
        "security" : {
            "authorization" : "disabled"
        }
    }
};
testGetCmdLineOpts({ noauth : "" }, expectedResult);

jsTest.log("Testing \"security.authorization\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_auth.json",
        "security" : {
            "authorization" : "enabled"
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/enable_auth.json" }, expectedResult);

jsTest.log("Testing with no explicit object check setting");
expectedResult = {
    "parsed" : { }
};
testGetCmdLineOpts({}, expectedResult);

print(baseName + " succeeded.");
