// dumprestore_24.js

load( './jstests/multiVersion/libs/verify_collection_data.js' )


// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore_24";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";
var testDbpath = MongoRunner.dataPath + testBaseName + "_dbpath_external/";

function multiVersionDumpRestoreTest(configObj) {

    // First sanity check the arguments in our configObj
    var requiredKeys = [
        'mongodSourceVersion',
        'mongodDestVersion',
        'mongoDumpVersion',
        'mongoRestoreVersion',
        'dumpDir',
        'testDbpath',
        'isDirectDump',
        'isDirectRestore'
    ]

    var i;

    for (i = 0; i < requiredKeys.length; i++) {
        assert(configObj.hasOwnProperty(requiredKeys[i]),
               "Missing required key: " + requiredKeys[i] + " in config object");
    }

    resetDbpath(configObj.dumpDir);
    resetDbpath(configObj.testDbpath);
    var mongodSource = MongoRunner.runMongod({ binVersion : configObj.mongodSourceVersion,
                                               setParameter : "textSearchEnabled=true",
                                               dbpath : configObj.testDbpath });
    var sourceDB = mongodSource.getDB(testBaseName);

    // Create generators to create collections with our seed data
    // Testing with both a capped collection and a normal collection
    var cappedCollGen = new CollectionDataGenerator({ "capped" : true });
    var collGen = new CollectionDataGenerator({ "capped" : false });

    // Create collections using the different generators
    var sourceCollCapped = createCollectionWithData(sourceDB, "cappedColl", cappedCollGen);
    var sourceColl = createCollectionWithData(sourceDB, "coll", collGen);

    // Record the current collection states for later validation
    var cappedCollValid = new CollectionDataValidator();
    cappedCollValid.recordCollectionData(sourceCollCapped);
    var collValid = new CollectionDataValidator();
    collValid.recordCollectionData(sourceColl);

    // Dump using the specified version of mongodump, either using the data files directly or from
    // the running mongod instance
    if (configObj.isDirectDump) {
        MongoRunner.stopMongod(mongodSource.port);
        MongoRunner.runMongoTool("mongodump", { out : configObj.dumpDir,
                                                binVersion : configObj.mongoDumpVersion,
                                                dbpath : configObj.testDbpath });
    }
    else {
        MongoRunner.runMongoTool("mongodump", { out : configObj.dumpDir,
                                                binVersion : configObj.mongoDumpVersion,
                                                host : mongodSource.host });
        MongoRunner.stopMongod(mongodSource.port);
    }

    // Restore using the specified version of mongorestore
    if (configObj.isDirectRestore) {

        // Clear out the dbpath we are restoring to
        resetDbpath(configObj.testDbpath);

        // Restore directly to the destination dbpath
        MongoRunner.runMongoTool("mongorestore", { dir : configObj.dumpDir,
                                                   binVersion : configObj.mongoRestoreVersion,
                                                   dbpath : configObj.testDbpath });

        // Start a mongod on the dbpath we just restored to
        // Need to pass the "restart" option otherwise the data files get cleared automatically
        mongodDest = MongoRunner.runMongod({ binVersion : configObj.mongodDestVersion,
                                             setParameter : "textSearchEnabled=true",
                                             dbpath : configObj.testDbpath,
                                             restart : true });
    }
    else {
        var mongodDest = MongoRunner.runMongod({ binVersion : configObj.mongodDestVersion,
                                                 setParameter : "textSearchEnabled=true" });

        MongoRunner.runMongoTool("mongorestore", { dir : configObj.dumpDir,
                                                   binVersion : configObj.mongoRestoreVersion,
                                                   host : mongodDest.host });
    }

    var destDB = mongodDest.getDB(testBaseName);

    // Get references to our destinations collections
    // XXX: These are in the global scope (no "var"), but they need to be global to be in scope for
    // the "assert.soon" calls below.
    destColl = destDB.getCollection("coll");
    destCollCapped = destDB.getCollection("cappedColl");

    // Wait until we actually have data or timeout
    assert.soon("destColl.findOne()", "no data after sleep");
    assert.soon("destCollCapped.findOne()", "no data after sleep");

    // Validate that our collections were properly restored
    assert(collValid.validateCollectionData(destColl));
    assert(cappedCollValid.validateCollectionData(destCollCapped));

    MongoRunner.stopMongod(mongodDest.port);
}

function getPermutationIterator(permsObj) {

    function getAllPermutations(permsObj) {

        // Split our permutations object into "first" and "rest"
        var gotFirst = false;
        var firstKey;
        var firstValues;
        var restObj = {};
        for (var key in permsObj) {
            if (permsObj.hasOwnProperty(key)) {
                if (gotFirst) {
                    restObj[key] = permsObj[key];
                }
                else {
                    firstKey = key;
                    firstValues = permsObj[key];
                    gotFirst = true;
                }
            }
        }

        // Our base case is an empty object, which just has a single permutation, "{}"
        if (!gotFirst) {
            return [{}];
        }

        // Iterate the possibilities for "first" and for each one recursively get all the
        // permutations for "rest"
        var resultPermObjs = [];
        var i = 0;
        var j = 0;
        for (i = 0; i < firstValues.length; i++) {
            var subPermObjs = getAllPermutations(restObj);
            for (j = 0; j < subPermObjs.length; j++) {
                subPermObjs[j][firstKey] = firstValues[i];
                resultPermObjs.push(subPermObjs[j]);
            }
        }
        return resultPermObjs;
    }

    var allPermutations = getAllPermutations(permsObj);
    var currentPermutation = 0;

    return {
        "next" : function () {
            return allPermutations[currentPermutation++];
        },
        "hasNext" : function () {
            return currentPermutation < allPermutations.length;
        }
    }
}

var testCasePermutations = {
    'mongodSourceVersion' : [ "latest", "2.4" ],
    'mongodDestVersion' :[ "latest", "2.4" ],
    'mongoDumpVersion' :[ "latest", "2.4" ],
    'mongoRestoreVersion' :[ "latest", "2.4" ],
    'dumpDir' : [ dumpDir ],
    'testDbpath' : [ testDbpath ],
    'isDirectDump' : [ true, false ],
    'isDirectRestore' : [ true, false ]
}
var testCaseCursor = getPermutationIterator(testCasePermutations);
while (testCaseCursor.hasNext()) {
    var testCase = testCaseCursor.next();
    print("Running multiversion mongodump mongorestore test:");
    printjson(testCase);
    multiVersionDumpRestoreTest(testCase);
}

print("dumprestore_24 success!");
