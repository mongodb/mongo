// Tests for mongodump options for excluding collections

var testBaseName = "jstests_tool_dumprestore_excludecollections";

var dumpDir = MongoRunner.dataPath + testBaseName + "_dump_external/";

var mongodSource = MongoRunner.runMongod();
var sourceDB = mongodSource.getDB(testBaseName);
var mongodDest = MongoRunner.runMongod();
var destDB = mongodDest.getDB(testBaseName);

jsTest.log("Inserting documents into source mongod");
sourceDB.test.insert({x: 1});
sourceDB.test2.insert({x: 2});
sourceDB.test3.insert({x: 3});
sourceDB.foo.insert({f: 1});
sourceDB.foo2.insert({f: 2});

jsTest.log("Testing incompabible option combinations");
resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool("mongodump",
                               {out: dumpDir, excludeCollection: "test", host: mongodSource.host});
assert.neq(ret, 0, "mongodump started successfully with --excludeCollection but no --db option");

resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool("mongodump", {
    out: dumpDir,
    db: testBaseName,
    collection: "foo",
    excludeCollection: "test",
    host: mongodSource.host
});
assert.neq(ret, 0, "mongodump started successfully with --excludeCollection and --collection");

resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool(
    "mongodump", {out: dumpDir, excludeCollectionsWithPrefix: "test", host: mongodSource.host});
assert.neq(
    ret,
    0,
    "mongodump started successfully with --excludeCollectionsWithPrefix but " + "no --db option");

resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool("mongodump", {
    out: dumpDir,
    db: testBaseName,
    collection: "foo",
    excludeCollectionsWithPrefix: "test",
    host: mongodSource.host
});
assert.neq(
    ret,
    0,
    "mongodump started successfully with --excludeCollectionsWithPrefix and " + "--collection");

jsTest.log("Testing proper behavior of collection exclusion");
resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool(
    "mongodump",
    {out: dumpDir, db: testBaseName, excludeCollection: "test", host: mongodSource.host});

ret = MongoRunner.runMongoTool("mongorestore", {dir: dumpDir, host: mongodDest.host});
assert.eq(ret, 0, "failed to run mongodump on expected successful call");
assert.eq(destDB.test.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test2.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.test2.findOne().x, 2, "Wrong value in document");
assert.eq(destDB.test3.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.test3.findOne().x, 3, "Wrong value in document");
assert.eq(destDB.foo.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo.findOne().f, 1, "Wrong value in document");
assert.eq(destDB.foo2.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo2.findOne().f, 2, "Wrong value in document");
destDB.dropDatabase();

resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool("mongodump", {
    out: dumpDir,
    db: testBaseName,
    excludeCollectionsWithPrefix: "test",
    host: mongodSource.host
});

ret = MongoRunner.runMongoTool("mongorestore", {dir: dumpDir, host: mongodDest.host});
assert.eq(ret, 0, "failed to run mongodump on expected successful call");
assert.eq(destDB.test.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test2.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test3.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.foo.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo.findOne().f, 1, "Wrong value in document");
assert.eq(destDB.foo2.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo2.findOne().f, 2, "Wrong value in document");
destDB.dropDatabase();

resetDbpath(dumpDir);
ret = MongoRunner.runMongoTool("mongodump", {
    out: dumpDir,
    db: testBaseName,
    excludeCollection: "foo",
    excludeCollectionsWithPrefix: "test",
    host: mongodSource.host
});

ret = MongoRunner.runMongoTool("mongorestore", {dir: dumpDir, host: mongodDest.host});
assert.eq(ret, 0, "failed to run mongodump on expected successful call");
assert.eq(destDB.test.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test2.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test3.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.foo.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.foo2.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo2.findOne().f, 2, "Wrong value in document");
destDB.dropDatabase();

// The --excludeCollection and --excludeCollectionsWithPrefix options can be specified multiple
// times, but that is not tested here because right now MongoRunners can only be configured using
// javascript objects which do not allow duplicate keys.  See SERVER-14220.

MongoRunner.stopMongod(mongodDest.port);
MongoRunner.stopMongod(mongodSource.port);

print(testBaseName + " success!");
