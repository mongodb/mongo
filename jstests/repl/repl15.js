// Test a case were an update can grow a document on master but growth is prevented on slave.
// SERVER-4939

if (0) {  // SERVER-4939

    function doTest(capped) {
        rt = new ReplTest("repl15tests");
        master = rt.start(true);
        if (capped) {
            master.getDB('d').createCollection('c', {capped: true, size: 5 * 1024});
        }
        mc = master.getDB('d')['c'];

        big = new Array(1000).toString();
        // Insert a document, then make it slightly smaller.
        mc.insert({a: big});
        mc.update({}, {$set: {a: 'b'}});

        slave = rt.start(false);
        sc = slave.getDB('d')['c'];

        // Slave will copy the smaller doc.
        assert.soon(function() {
            return sc.count({a: 'b'}) > 0;
        });

        // Update the primary doc to its original size.
        mc.update({}, {$set: {a: big}});

        // Wait for secondary to clone the update.
        assert.soon(function() {
            return sc.count({a: big}) > 0;
        });

        rt.stop();
    }

    doTest(false);
    doTest(true);
}
