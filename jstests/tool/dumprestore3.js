// merizodump/merizoexport from primary should succeed.  merizorestore and merizoimport to a
// secondary node should fail.

(function() {
    // Skip this test if running with --nojournal and WiredTiger.
    if (jsTest.options().noJournal &&
        (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
        print("Skipping test because running WiredTiger without journaling isn't a valid" +
              " replica set configuration");
        return;
    }

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

    jsTestLog("merizodump from primary");
    var data = MongoRunner.dataDir + "/dumprestore3-other1/";
    resetDbpath(data);
    var ret = MongoRunner.runMongoTool("merizodump", {
        host: primary.host,
        out: data,
    });
    assert.eq(ret, 0, "merizodump should exit w/ 0 on primary");

    jsTestLog("try merizorestore to secondary");
    ret = MongoRunner.runMongoTool("merizorestore", {
        host: secondary.host,
        dir: data,
    });
    assert.neq(ret, 0, "merizorestore should exit w/ 1 on secondary");

    jsTestLog("merizoexport from primary");
    dataFile = MongoRunner.dataDir + "/dumprestore3-other2.json";
    ret = MongoRunner.runMongoTool("merizoexport", {
        host: primary.host,
        out: dataFile,
        db: "foo",
        collection: "bar",
    });
    assert.eq(ret, 0, "merizoexport should exit w/ 0 on primary");

    jsTestLog("merizoimport from secondary");
    ret = MongoRunner.runMongoTool("merizoimport", {
        host: secondary.host,
        file: dataFile,
    });
    assert.neq(ret, 0, "merizoimport should exit w/ 1 on secondary");

    jsTestLog("stopSet");
    replTest.stopSet();
    jsTestLog("SUCCESS");
}());
