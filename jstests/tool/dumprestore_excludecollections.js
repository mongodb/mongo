// Tests for bongodump options for excluding collections

var testBaseName = "jstests_tool_dumprestore_excludecollections";

var dumpDir = BongoRunner.dataPath + testBaseName + "_dump_external/";

var bongodSource = BongoRunner.runBongod();
var sourceDB = bongodSource.getDB(testBaseName);
var bongodDest = BongoRunner.runBongod();
var destDB = bongodDest.getDB(testBaseName);

jsTest.log("Inserting documents into source bongod");
sourceDB.test.insert({x: 1});
sourceDB.test2.insert({x: 2});
sourceDB.test3.insert({x: 3});
sourceDB.foo.insert({f: 1});
sourceDB.foo2.insert({f: 2});

jsTest.log("Testing incompabible option combinations");
resetDbpath(dumpDir);
ret = BongoRunner.runBongoTool("bongodump",
                               {out: dumpDir, excludeCollection: "test", host: bongodSource.host});
assert.neq(ret, 0, "bongodump started successfully with --excludeCollection but no --db option");

resetDbpath(dumpDir);
ret = BongoRunner.runBongoTool("bongodump", {
    out: dumpDir,
    db: testBaseName,
    collection: "foo",
    excludeCollection: "test",
    host: bongodSource.host
});
assert.neq(ret, 0, "bongodump started successfully with --excludeCollection and --collection");

resetDbpath(dumpDir);
ret = BongoRunner.runBongoTool(
    "bongodump", {out: dumpDir, excludeCollectionsWithPrefix: "test", host: bongodSource.host});
assert.neq(
    ret,
    0,
    "bongodump started successfully with --excludeCollectionsWithPrefix but " + "no --db option");

resetDbpath(dumpDir);
ret = BongoRunner.runBongoTool("bongodump", {
    out: dumpDir,
    db: testBaseName,
    collection: "foo",
    excludeCollectionsWithPrefix: "test",
    host: bongodSource.host
});
assert.neq(
    ret,
    0,
    "bongodump started successfully with --excludeCollectionsWithPrefix and " + "--collection");

jsTest.log("Testing proper behavior of collection exclusion");
resetDbpath(dumpDir);
ret = BongoRunner.runBongoTool(
    "bongodump",
    {out: dumpDir, db: testBaseName, excludeCollection: "test", host: bongodSource.host});

ret = BongoRunner.runBongoTool("bongorestore", {dir: dumpDir, host: bongodDest.host});
assert.eq(ret, 0, "failed to run bongodump on expected successful call");
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
ret = BongoRunner.runBongoTool("bongodump", {
    out: dumpDir,
    db: testBaseName,
    excludeCollectionsWithPrefix: "test",
    host: bongodSource.host
});

ret = BongoRunner.runBongoTool("bongorestore", {dir: dumpDir, host: bongodDest.host});
assert.eq(ret, 0, "failed to run bongodump on expected successful call");
assert.eq(destDB.test.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test2.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test3.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.foo.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo.findOne().f, 1, "Wrong value in document");
assert.eq(destDB.foo2.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo2.findOne().f, 2, "Wrong value in document");
destDB.dropDatabase();

resetDbpath(dumpDir);
ret = BongoRunner.runBongoTool("bongodump", {
    out: dumpDir,
    db: testBaseName,
    excludeCollection: "foo",
    excludeCollectionsWithPrefix: "test",
    host: bongodSource.host
});

ret = BongoRunner.runBongoTool("bongorestore", {dir: dumpDir, host: bongodDest.host});
assert.eq(ret, 0, "failed to run bongodump on expected successful call");
assert.eq(destDB.test.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test2.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.test3.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.foo.count(), 0, "Found documents in collection that we excluded");
assert.eq(destDB.foo2.count(), 1, "Did not find document in collection that we did not exclude");
assert.eq(destDB.foo2.findOne().f, 2, "Wrong value in document");
destDB.dropDatabase();

// The --excludeCollection and --excludeCollectionsWithPrefix options can be specified multiple
// times, but that is not tested here because right now BongoRunners can only be configured using
// javascript objects which do not allow duplicate keys.  See SERVER-14220.

BongoRunner.stopBongod(bongodDest.port);
BongoRunner.stopBongod(bongodSource.port);

print(testBaseName + " success!");
