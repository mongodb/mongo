var baseName = "jstests_disk_index_options";

function removeOptionsAddedByFramework(getCmdLineOptsResult) {
    // Remove options that we are not interested in checking, but that get set by the test
    delete getCmdLineOptsResult.parsed.setParameter
    delete getCmdLineOptsResult.parsed.storage.dbPath
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

jsTest.log("Testing \"noIndexBuildRetry\" command line option");
var expectedResult = {
    "parsed" : {
        "storage" : {
            "indexBuildRetry" : false
        }
    }
};
testGetCmdLineOpts({ noIndexBuildRetry : "" }, expectedResult);

jsTest.log("Testing \"storage.indexBuildRetry\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_indexbuildretry.json",
        "storage" : {
            "indexBuildRetry" : true
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/enable_indexbuildretry.json" },
                   expectedResult);

jsTest.log("Testing with no explicit index option setting");
expectedResult = {
    "parsed" : {
        "storage" : { }
    }
};
testGetCmdLineOpts({}, expectedResult);

print(baseName + " succeeded.");
