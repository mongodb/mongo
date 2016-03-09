/**
 * This test is only for WiredTiger storageEngine
 * Test nojournal and fsync with wiredTiger standalone.
 * Add some data and kill -9, once after fsync, once without fsync
 * If no fsync, data should go away after restart.
 * If fsync, should recover and data should be present after restart.
 */

function writeDataAndRestart(doFsync) {
    jsTestLog("add some data");
    for (var i = 0; i < 100; i++) {
        conn.getDB(name).foo.insert({x: i});
    }

    if (doFsync) {
        jsTestLog("run fsync on the node");
        assert.commandWorked(conn.getDB("admin").runCommand({fsync: 1}));
    }

    jsTestLog("kill -9");
    MongoRunner.stopMongod(conn, /*signal*/ 9);

    jsTestLog("restart node");
    conn = MongoRunner.runMongod({
        restart: true,
        port: conn.port,
        cleanData: false,
        storageEngine: "wiredTiger",
        nojournal: ""
    });
    return conn;
}

// This test can only be run if the storageEngine is wiredTiger
if (jsTest.options().storageEngine && jsTest.options().storageEngine !== "wiredTiger") {
    jsTestLog("Skipping test because storageEngine is not wiredTiger");
} else {
    var name = "wt_nojournal_fsync";

    jsTestLog("run mongod without journaling");
    conn = MongoRunner.runMongod({storageEngine: "wiredTiger", nojournal: ""});

    // restart node without fsync and --nojournal.  Data should not be there after restart
    conn = writeDataAndRestart(false);
    jsTestLog("check data is not in collection foo");
    assert.eq(conn.getDB(name).foo.count(), 0);

    // restart node with fsync and --nojournal.  Data should be there after restart
    conn = writeDataAndRestart(true);
    jsTestLog("check data is in collection foo");
    assert.eq(conn.getDB(name).foo.count(), 100);

    MongoRunner.stopMongod(conn);
    jsTestLog("Success!");
}
