// Test deduping of new documents without an _id index
// SERVER-14132

if (0) {
    function doTest(insert) {
        rt = new ReplTest("repl16tests");
        master = rt.start(true);
        master.getDB('d').createCollection('c', {capped: true, size: 5 * 1024, autoIndexId: false});
        mc = master.getDB('d')['c'];

        insert({_id: 1});
        insert({_id: 2});

        slave = rt.start(false);
        sc = slave.getDB('d')['c'];

        // Wait for the slave to copy the documents.
        assert.soon(function() {
            return sc.count() == 2;
        });

        insert({_id: 1});
        insert({_id: 2});
        insert({_id: 3});
        assert.eq(5, mc.count());

        // Wait for the slave to apply the operations.
        assert.soon(function() {
            return sc.count() == 5;
        });

        rt.stop();
    }

    function insertWithIds(obj) {
        mc.insert(obj);
    }

    doTest(insertWithIds);
}
