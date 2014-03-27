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

// Object Check
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

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed" : {
        "net" : { }
    }
};
testGetCmdLineOpts({}, expectedResult);



// HTTP Interface
jsTest.log("Testing \"httpinterface\" command line option");
var expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOpts({ httpinterface : "" }, expectedResult);

jsTest.log("Testing \"nohttpinterface\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "enabled" : false
            }
        }
    }
};
testGetCmdLineOpts({ nohttpinterface : "" }, expectedResult);

jsTest.log("Testing implicit enabling of http interface with \"jsonp\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "JSONPEnabled" : true,
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOpts({ jsonp : "" }, expectedResult);

jsTest.log("Testing implicit enabling of http interface with \"rest\" command line option");
expectedResult = {
    "parsed" : {
        "net" : {
            "http" : {
                "RESTInterfaceEnabled" : true,
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOpts({ rest : "" }, expectedResult);

jsTest.log("Testing \"net.http.enabled\" config file option");
expectedResult = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_httpinterface.json",
        "net" : {
            "http" : {
                "enabled" : true
            }
        }
    }
};
testGetCmdLineOpts({ config : "jstests/libs/config_files/enable_httpinterface.json" }, expectedResult);

jsTest.log("Testing with no explicit network option setting");
expectedResult = {
    "parsed" : {
        "net" : { }
    }
};
testGetCmdLineOpts({}, expectedResult);

print(baseName + " succeeded.");
