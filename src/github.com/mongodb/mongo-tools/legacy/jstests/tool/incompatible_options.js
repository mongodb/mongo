// Tests to ensure that incompatible options cause a failure to startup.



// Make sure --host and --dbpath options are incompatible.

// The base name to use for various things in the test, including the dbpath and the database name
var testBaseName = "jstests_tool_incompatible_options";

// Paths to external directories to be used to store dump files
var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";
var dumpFile = MongoRunner.dataPath + testBaseName + "_export_external.json";
var testDbpath = MongoRunner.dataPath + testBaseName + "_dbpath_external/";

resetDbpath(dumpDir);
resetDbpath(testDbpath);

// First, start and stop the mongod we are using for the direct dump
var mongodDirect = MongoRunner.runMongod({ dbpath : testDbpath });
mongodDirect.getDB(testBaseName).getCollection("test").insert({x:1});
MongoRunner.stopMongod(mongodDirect.port);

// Next, start the mongod we are using for the network dump
var mongodSource = MongoRunner.runMongod();
var sourceDB = mongodSource.getDB(testBaseName);

// Test that mongodump with both --host and --dbpath fails
var ret = MongoRunner.runMongoTool("mongodump", { out : dumpDir,
                                                  dbpath : testDbpath,
                                                  host : mongodSource.host });
assert.neq(ret, 0, "mongodump started successfully with both --host and --dbpath");

// Test that mongorestore with both --host and --dbpath fails, but succeeds otherwise
ret = MongoRunner.runMongoTool("mongodump", { out : dumpDir, dbpath : testDbpath });
assert.eq(ret, 0, "failed to run mongorestore on expected successful call");
ret = MongoRunner.runMongoTool("mongorestore", { dir : dumpDir, host : mongodSource.host });
assert.eq(ret, 0, "failed to run mongodump on expected successful call");
mongodSource.getDB(testBaseName).dropDatabase();
ret = MongoRunner.runMongoTool("mongorestore", { dir : dumpDir,
                                                 dbpath : testDbpath,
                                                 host : mongodSource.host });
assert.neq(ret, 0, "mongorestore started successfully with both --host and --dbpath");

// Test that mongoexport with both --host and --dbpath fails
ret = MongoRunner.runMongoTool("mongoexport", { out : dumpFile,
                                                db : testBaseName,
                                                collection : "test",
                                                dbpath : testDbpath,
                                                host : mongodSource.host });
assert.neq(ret, 0, "mongoexport started successfully with both --host and --dbpath");

// Test that mongoimport with both --host and --dbpath fails, but succeeds otherwise
ret = MongoRunner.runMongoTool("mongoexport", { out : dumpFile,
                                                db : testBaseName,
                                                collection : "test",
                                                dbpath : testDbpath });
assert.eq(ret, 0, "failed to run mongoexport on expected successful call");
ret = MongoRunner.runMongoTool("mongoimport", { file : dumpFile,
                                                db : testBaseName,
                                                collection : "test",
                                                host : mongodSource.host });
assert.eq(ret, 0, "failed to run mongoimport on expected successful call");
mongodSource.getDB(testBaseName).dropDatabase();
ret = MongoRunner.runMongoTool("mongoimport", { file : dumpFile,
                                                db : testBaseName,
                                                collection : "test",
                                                dbpath : testDbpath,
                                                host : mongodSource.host });
assert.neq(ret, 0, "mongoimport started successfully with both --host and --dbpath");
MongoRunner.stopMongod(mongodSource.port);


print(testBaseName + " success!");
