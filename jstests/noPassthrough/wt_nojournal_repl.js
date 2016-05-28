/**
 * This test is only for the WiredTiger storageEngine
 * Test nojournal option with wiredTiger replset.  This test will fail with mmap because
 * unclean shutdowns are not safe.  If running without a journal, WT starts up from last
 * valid checkpoint, so should recover.
 *
 * Start a set.
 * Insert data into collection foo
 * fsync secondary member 1
 * Kill -9 secondary member 1
 * Add some more data in a new collection.
 * Restart member 1.
 * Check that it syncs from the last checkpoint and data is there.
 */

// used to parse RAM log file
var contains = function(logLines, func) {
    var i = logLines.length;
    while (i--) {
        printjson(logLines[i]);
        if (func(logLines[i])) {
            return true;
        }
    }
    return false;
};

// This test can only be run if the storageEngine is wiredTiger
if (jsTest.options().storageEngine && jsTest.options().storageEngine !== "wiredTiger") {
    jsTestLog("Skipping test because storageEngine is not wiredTiger");
} else {
    var name = "wt_nojournal_repl";
    var replTest = new ReplSetTest({
        name: name,
        nodes: 3,
        oplogSize: 2,
        nodeOptions: {
            nojournal: "",
            storageEngine: "wiredTiger",
        }
    });
    var nodes = replTest.startSet();

    // make sure node 0 becomes primary initially
    var config = replTest.getReplSetConfig();
    config.members[0].priority = 1;
    replTest.initiate(config);

    var masterDB = replTest.getPrimary().getDB("test");
    var secondary1 = replTest.liveNodes.slaves[0];

    jsTestLog("add some data to collection foo");
    for (var i = 0; i < 100; i++) {
        masterDB.foo.insert({x: i});
    }
    replTest.awaitReplication();
    assert.eq(secondary1.getDB("test").foo.count(), 100);

    jsTestLog("run fsync on the secondary to ensure it remains after restart");
    assert.commandWorked(secondary1.getDB("admin").runCommand({fsync: 1}));

    jsTestLog("kill -9 secondary 1");
    MongoRunner.stopMongod(secondary1.port, /*signal*/ 9);

    jsTestLog("add some data to a new collection bar");
    for (var i = 0; i < 100; i++) {
        masterDB.bar.insert({x: i});
    }

    jsTestLog("restart secondary 1 and let it catch up");
    secondary1 = replTest.restart(1);
    replTest.awaitReplication();

    // Test that the restarted secondary did NOT do an initial sync by checking the log
    var res = secondary1.adminCommand({getLog: "global"});
    assert(!contains(res.log, function(v) {
        return v.indexOf("initial sync") != -1;
    }));

    jsTestLog("check data is in both collections");
    assert.eq(secondary1.getDB("test").foo.count(), 100);
    assert.eq(secondary1.getDB("test").bar.count(), 100);

    jsTestLog("Success!");
    replTest.stopSet();
}
