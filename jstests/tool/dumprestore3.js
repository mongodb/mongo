// bongodump/bongoexport from primary should succeed.  bongorestore and bongoimport to a
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

jsTestLog("bongodump from primary");
var data = BongoRunner.dataDir + "/dumprestore3-other1/";
resetDbpath(data);
var ret = BongoRunner.runBongoTool("bongodump", {
    host: primary.host,
    out: data,
});
assert.eq(ret, 0, "bongodump should exit w/ 0 on primary");

jsTestLog("try bongorestore to secondary");
ret = BongoRunner.runBongoTool("bongorestore", {
    host: secondary.host,
    dir: data,
});
assert.neq(ret, 0, "bongorestore should exit w/ 1 on secondary");

jsTestLog("bongoexport from primary");
dataFile = BongoRunner.dataDir + "/dumprestore3-other2.json";
ret = BongoRunner.runBongoTool("bongoexport", {
    host: primary.host,
    out: dataFile,
    db: "foo",
    collection: "bar",
});
assert.eq(ret, 0, "bongoexport should exit w/ 0 on primary");

jsTestLog("bongoimport from secondary");
ret = BongoRunner.runBongoTool("bongoimport", {
    host: secondary.host,
    file: dataFile,
});
assert.neq(ret, 0, "bongoimport should exit w/ 1 on secondary");

jsTestLog("stopSet");
replTest.stopSet();
jsTestLog("SUCCESS");
