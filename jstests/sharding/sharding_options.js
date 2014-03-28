var baseName = "jstests_sharding_sharding_options";

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

// Move Paranoia
jsTest.log("Testing \"moveParanoia\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "archiveMovedChunks" : true
        }
    }
};
testGetCmdLineOpts({ moveParanoia : "" }, expectedResult);

jsTest.log("Testing \"noMoveParanoia\" command line option");
expectedResult = {
    "parsed" : {
        "sharding" : {
            "archiveMovedChunks" : false
        }
    }
};
testGetCmdLineOpts({ noMoveParanoia : "" }, expectedResult);

jsTest.log("Testing \"sharding.archiveMovedChunks\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_paranoia.json",
        "sharding" : {
            "archiveMovedChunks" : true
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/enable_paranoia.json" }, expectedResult);



// Sharding Role
jsTest.log("Testing \"configsvr\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "clusterRole" : "configsvr"
        }
    }
};
testGetCmdLineOpts({ configsvr : "" }, expectedResult);

jsTest.log("Testing \"shardsvr\" command line option");
expectedResult = {
    "parsed" : {
        "sharding" : {
            "clusterRole" : "shardsvr"
        }
    }
};
testGetCmdLineOpts({ shardsvr : "" }, expectedResult);

jsTest.log("Testing \"sharding.clusterRole\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_shardingrole.json",
        "sharding" : {
            "clusterRole" : "configsvr"
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/set_shardingrole.json" }, expectedResult);

jsTest.log("Testing with no explicit sharding option setting");
expectedResult = {
    "parsed" : { }
};
testGetCmdLineOpts({}, expectedResult);



print(baseName + " succeeded.");
