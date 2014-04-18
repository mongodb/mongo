var baseName = "jstests_dur_journaling_options";

function removeOptionsAddedByFramework(getCmdLineOptsResult) {
    // Remove options that we are not interested in checking, but that get set by the test
    delete getCmdLineOptsResult.parsed.setParameter
    delete getCmdLineOptsResult.parsed.storage.dbPath
    delete getCmdLineOptsResult.parsed.net
    delete getCmdLineOptsResult.parsed.fastsync
    return getCmdLineOptsResult;
}

jsTest.log("Testing \"dur\" command line option");
var mongodSource = MongoRunner.runMongod({ dur : "" });
var getCmdLineOptsExpected = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : true
            }
        }
    }
};

var getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");
printjson(getCmdLineOptsResult);
getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);
assert.docEq(getCmdLineOptsResult.parsed, getCmdLineOptsExpected.parsed);
MongoRunner.stopMongod(mongodSource.port);

jsTest.log("Testing \"nodur\" command line option");
mongodSource = MongoRunner.runMongod({ nodur : "" });
getCmdLineOptsExpected = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : false
            }
        }
    }
};

getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");
printjson(getCmdLineOptsResult);
getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);
assert.docEq(getCmdLineOptsResult.parsed, getCmdLineOptsExpected.parsed);
MongoRunner.stopMongod(mongodSource.port);

jsTest.log("Testing \"journal\" command line option");
mongodSource = MongoRunner.runMongod({ journal : "" });
getCmdLineOptsExpected = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : true
            }
        }
    }
};

getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");
printjson(getCmdLineOptsResult);
getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);
assert.docEq(getCmdLineOptsResult.parsed, getCmdLineOptsExpected.parsed);
MongoRunner.stopMongod(mongodSource.port);

jsTest.log("Testing \"nojournal\" command line option");
mongodSource = MongoRunner.runMongod({ nojournal : "" });
getCmdLineOptsExpected = {
    "parsed" : {
        "storage" : {
            "journal" : {
                "enabled" : false
            }
        }
    }
};

getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");
printjson(getCmdLineOptsResult);
getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);
assert.docEq(getCmdLineOptsResult.parsed, getCmdLineOptsExpected.parsed);
MongoRunner.stopMongod(mongodSource.port);

jsTest.log("Testing \"storage.journal.enabled\" config file option");
mongodSource = MongoRunner.runMongod({ config : "jstests/libs/config_files/enable_journal.json" });
getCmdLineOptsExpected = {
    "parsed" : {
        "config" : "jstests/libs/config_files/enable_journal.json",
        "storage" : {
            "journal" : {
                "enabled" : false
            }
        }
    }
};

getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");
printjson(getCmdLineOptsResult);
getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);
assert.docEq(getCmdLineOptsResult.parsed, getCmdLineOptsExpected.parsed);
MongoRunner.stopMongod(mongodSource.port);

jsTest.log("Testing with no explicit journal setting");
mongodSource = MongoRunner.runMongod();
getCmdLineOptsExpected = {
    "parsed" : {
        "storage" : { }
    }
};

getCmdLineOptsResult = mongodSource.adminCommand("getCmdLineOpts");
printjson(getCmdLineOptsResult);
getCmdLineOptsResult = removeOptionsAddedByFramework(getCmdLineOptsResult);
assert.docEq(getCmdLineOptsResult.parsed, getCmdLineOptsExpected.parsed);
MongoRunner.stopMongod(mongodSource.port);

print(baseName + " succeeded.");
