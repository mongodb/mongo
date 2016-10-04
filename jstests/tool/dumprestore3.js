// mongodump/mongoexport from primary should succeed.  mongorestore and mongoimport to a
// secondary node should fail.

var name = "dumprestore3";

var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
var primary = replTest.getPrimary();
var secondary = replTest.getSecondary();

jsTestLog("populate primary");
var foo = primary.getDB("foo");
for (i = 0; i < 20; i++) {
    foo.bar.insert({x: i, y: "abc"});
}

jsTestLog("wait for secondary");
replTest.awaitReplication();

jsTestLog("mongodump from primary");
var data = MongoRunner.dataDir + "/dumprestore3-other1/";
resetDbpath(data);
var ret = MongoRunner.runMongoTool("mongodump", {
    host: primary.host,
    out: data,
});
assert.eq(ret, 0, "mongodump should exit w/ 0 on primary");

jsTestLog("try mongorestore to secondary");
ret = MongoRunner.runMongoTool("mongorestore", {
    host: secondary.host,
    dir: data,
});
assert.neq(ret, 0, "mongorestore should exit w/ 1 on secondary");

jsTestLog("mongoexport from primary");
dataFile = MongoRunner.dataDir + "/dumprestore3-other2.json";
ret = MongoRunner.runMongoTool("mongoexport", {
    host: primary.host,
    out: dataFile,
    db: "foo",
    collection: "bar",
});
assert.eq(ret, 0, "mongoexport should exit w/ 0 on primary");

jsTestLog("mongoimport from secondary");
ret = MongoRunner.runMongoTool("mongoimport", {
    host: secondary.host,
    file: dataFile,
});
assert.neq(ret, 0, "mongoimport should exit w/ 1 on secondary");

jsTestLog("stopSet");
replTest.stopSet();
jsTestLog("SUCCESS");
