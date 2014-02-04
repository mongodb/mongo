// Tests that you can dump user data from 2.4 and restore it to a clean 2.6 system, and that the
// current version of mongodump/mongorestore still support dumping/restoring to and from 2.4.


// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_dumprestore_24_to_26";
var dumpDir = MongoRunner.getAndPrepareDumpDirectory(testBaseName);

function multiVersionDumpRestoreTest(opts) {
    resetDbpath(dumpDir);
    var mongodSource = MongoRunner.runMongod({ binVersion : opts.mongodSourceVersion,
                                               setParameter : "textSearchEnabled=true" });
    var mongodDest = MongoRunner.runMongod({ binVersion : opts.mongodDestVersion,
                                             setParameter : "textSearchEnabled=true" });
    var sourceDB = mongodSource.getDB(testBaseName);
    var sourceAdmin = mongodSource.getDB("admin");
    var destDB = mongodDest.getDB(testBaseName);
    var destAdmin = mongodDest.getDB("admin");

    sourceAdmin.addUser({user: 'adminUser', pwd: 'pwd', roles: ['userAdminAnyDatabase']});
    sourceDB.addUser({user: 'user', pwd: 'pwd', roles: ['readWrite']});

    // Dump using the specified version of mongodump
    MongoRunner.runMongoTool("mongodump", { out : dumpDir, binVersion : opts.mongoDumpVersion,
                                            host : mongodSource.host });

    // Drop the databases
    sourceDB.dropDatabase();
    sourceAdmin.dropDatabase();

    // Restore using the specified version of mongorestore
    MongoRunner.runMongoTool("mongorestore", { dir : dumpDir, binVersion : opts.mongoRestoreVersion,
                                               host : mongodDest.host });

    // Wait until we actually have data or timeout
    assert.soon(function() { return destAdmin.system.users.findOne(); }, "no data after sleep");
    assert.eq(1, destDB.system.users.count());
    assert.eq('user', destDB.system.users.findOne().user);
    assert.eq(1, destAdmin.system.users.count());
    assert.eq('adminUser', destAdmin.system.users.findOne().user);

    if (opts.mongodDestVersion == "latest") {
        var schemaVersion =
            destAdmin.runCommand({getParameter: 1, authSchemaVersion: 1}).authSchemaVersion;
        if (opts.mongodSourceVersion == "2.4") {
            assert.eq(1, schemaVersion);
        } else if (opts.mongodSourceVersion == "latest") {
            assert.eq(3, schemaVersion);
        }
    }

    MongoRunner.stopMongod(mongodSource);
    MongoRunner.stopMongod(mongodDest);
}

multiVersionDumpRestoreTest({mongodSourceVersion: "2.4",
                             mongodDestVersion: "latest",
                             mongoDumpVersion: "latest",
                             mongoRestoreVersion: "latest"});

multiVersionDumpRestoreTest({mongodSourceVersion: "2.4",
                             mongodDestVersion: "2.4",
                             mongoDumpVersion: "latest",
                             mongoRestoreVersion: "latest"});


print("dumprestore_24_to_26 success!");
