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

function testGetCmdLineOptsMongod(mongoRunnerConfig, expectedResult) {

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

function testGetCmdLineOptsMongos(mongoRunnerConfig, expectedResult) {

    // Start mongod with options
    var mongod = MongoRunner.runMongod();

    // Add configdb option
    mongoRunnerConfig['configdb'] = mongod.host;

    // Start mongos connected to mongod
    var mongos = MongoRunner.runMongos(mongoRunnerConfig);

    // Get the parsed options
    var getCmdLineOptsResult = mongos.adminCommand("getCmdLineOpts");
    printjson(getCmdLineOptsResult);

    // Remove options added by the test framework
    getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);

    // Remove the configdb option
    delete getCmdLineOptsResult.parsed.sharding.configDB;

    // Make sure the options are equal to what we expect
    assert.docEq(getCmdLineOptsResult.parsed, expectedResult.parsed);

    // Cleanup
    MongoRunner.stopMongod(mongod.port);
    MongoRunner.stopMongos(mongos.port);
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
testGetCmdLineOptsMongod({ moveParanoia : "" }, expectedResult);

jsTest.log("Testing \"noMoveParanoia\" command line option");
expectedResult = {
    "parsed" : {
        "sharding" : {
            "archiveMovedChunks" : false
        }
    }
};
testGetCmdLineOptsMongod({ noMoveParanoia : "" }, expectedResult);

jsTest.log("Testing \"sharding.archiveMovedChunks\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_paranoia.json",
        "sharding" : {
            "archiveMovedChunks" : true
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/enable_paranoia.json" }, expectedResult);



// Sharding Role
jsTest.log("Testing \"configsvr\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "clusterRole" : "configsvr"
        }
    }
};
testGetCmdLineOptsMongod({ configsvr : "" }, expectedResult);

jsTest.log("Testing \"shardsvr\" command line option");
expectedResult = {
    "parsed" : {
        "sharding" : {
            "clusterRole" : "shardsvr"
        }
    }
};
testGetCmdLineOptsMongod({ shardsvr : "" }, expectedResult);

jsTest.log("Testing \"sharding.clusterRole\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/set_shardingrole.json",
        "sharding" : {
            "clusterRole" : "configsvr"
        }
    }
};
testGetCmdLineOptsMongod({ config : "jstests/libs/config_files/set_shardingrole.json" }, expectedResult);



// Auto Splitting
jsTest.log("Testing \"noAutoSplit\" command line option");
var expectedResult = {
    "parsed" : {
        "sharding" : {
            "autoSplit" : false
        }
    }
};
testGetCmdLineOptsMongos({ noAutoSplit : "" }, expectedResult);

jsTest.log("Testing \"sharding.autoSplit\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_autosplit.json",
        "sharding" : {
            "autoSplit" : true
        }
    }
};
testGetCmdLineOptsMongos({ config : "jstests/libs/config_files/enable_autosplit.json" },
                         expectedResult);



print(baseName + " succeeded.");
