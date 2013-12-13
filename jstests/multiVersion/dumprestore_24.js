// dumprestore_24.js

load( './jstests/multiVersion/libs/verify_collection_data.js' )


// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore_24";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_external/";

function multiVersionDumpRestoreTest(mongodSourceVersion,
                                     mongodDestVersion,
                                     mongoDumpVersion,
                                     mongoRestoreVersion,
                                     dumpDir) {
    resetDbpath(dumpDir);
    var mongodSource = MongoRunner.runMongod({ binVersion : mongodSourceVersion,
                                               setParameter : "textSearchEnabled=true" });
    var mongodDest = MongoRunner.runMongod({ binVersion : mongodDestVersion,
                                             setParameter : "textSearchEnabled=true" });
    var sourceDB = mongodSource.getDB(testBaseName);
    var destDB = mongodDest.getDB(testBaseName);

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

    // Get references to our destinations collections
    // XXX: These are in the global scope (no "var"), but they need to be global to be in scope for
    // the "assert.soon" calls below.
    destColl = destDB.getCollection("coll");
    destCollCapped = destDB.getCollection("cappedColl");

    // Dump using the specified version of mongodump
    MongoRunner.runMongoTool("mongodump", { out : dumpDir, binVersion : mongoDumpVersion,
                                            host : mongodSource.host });

    // Drop the collections and make sure they are actually empty
    destColl.drop();
    destCollCapped.drop();
    assert.eq(0, destColl.count(), "after drop");
    assert.eq(0, destCollCapped.count(), "after drop");

    // Dump using the specified version of mongorestore
    MongoRunner.runMongoTool("mongorestore", { dir : dumpDir, binVersion : mongoRestoreVersion,
                                               host : mongodDest.host });

    // Wait until we actually have data or timeout
    assert.soon("destColl.findOne()", "no data after sleep");
    assert.soon("destCollCapped.findOne()", "no data after sleep");

    // Validate that our collections were properly restored
    assert(collValid.validateCollectionData(destColl));
    assert(cappedCollValid.validateCollectionData(destCollCapped));
}

var versionsToTest = [ "latest", "2.4" ]

for (var i = 0; i < versionsToTest.length; i++) {
    for (var j = 0; j < versionsToTest.length; j++) {
        for (var k = 0; k < versionsToTest.length; k++) {
            for (var l = 0; l < versionsToTest.length; l++) {
                multiVersionDumpRestoreTest(versionsToTest[i], //mongodSourceVersion,
                                            versionsToTest[j], //mongodDestVersion,
                                            versionsToTest[k], //mongoDumpVersion,
                                            versionsToTest[l], //mongoRestoreVersion,
                                            dumpDir //dumpDir
                                        );
            }
        }
    }
}

print("dumprestore_24 success!");
