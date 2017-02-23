// Test collection rename during initial sync.
// SERVER-4941

if (0) {  // SERVER-4941

    rt = new ReplTest("repl17tests");

    master = rt.start(true);
    md = master.getDB('d');

    for (i = 0; i < 1000; ++i) {
        md['' + i].save({});
    }

    slave = rt.start(false);
    sd = slave.getDB('d');

    function checkSlaveCount(collection, expectedCount) {
        var count = sd[collection].count();
        var debug = false;
        if (debug) {
            print(collection + ': ' + count);
        }
        return count == expectedCount;
    }

    // Wait for the slave to start cloning
    assert.soon(function() {
        return checkSlaveCount('0', 1);
    });

    assert.commandWorked(md['999'].renameCollection('renamed'));

    // Check for renamed collection on slave.
    assert.soon(function() {
        return checkSlaveCount('999', 0) && checkSlaveCount('renamed', 1);
    });

    rt.stop();
}
