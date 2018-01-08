/**
 * SERVER-31099 WiredTiger uses lookaside (LAS) file when the cache contents are
 * pinned and can not be evicted for example in the case of a delayed slave
 * with read concern majority. This test inserts enough data where not using the
 * lookaside file results in a stall we can't recover from.
 *
 * This test is labeled resource intensive because its total io_write is 900MB.
 * @tags: [resource_intensive]
 */
(function() {
    "use strict";

    // Skip this test if not running with the "wiredTiger" storage engine.
    var storageEngine = jsTest.options().storageEngine || "wiredTiger";
    if (storageEngine !== "wiredTiger") {
        print('Skipping test because storageEngine is not "wiredTiger"');
        return;
    } else if (jsTest.options().wiredTigerCollectionConfigString === "type=lsm") {
        // Readers of old data, such as a lagged secondary, can lead to stalls when using
        // WiredTiger's LSM tree.
        print("WT-3742: Skipping test because we're running with WiredTiger's LSM tree");
        return;
    } else {
        var rst = new ReplSetTest({
            nodes: 2,
            // We are going to insert at least 100 MB of data with a long slave
            // delay. Configure an appropriately large oplog size.
            oplogSize: 200,
        });

        var conf = rst.getReplSetConfig();
        conf.members[1].votes = 1;
        conf.members[1].priority = 0;
        conf.members[1].slaveDelay = 24 * 60 * 60;

        rst.startSet();
        rst.initiateWithAnyNodeAsPrimary(conf);
        var master = rst.getPrimary();  // Waits for PRIMARY state.

        // Reconfigure primary with a small cache size so less data needs to be
        // inserted to make the cache full while trying to trigger a stall.
        assert.commandWorked(master.adminCommand(
            {setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=100MB"}));

        var coll = master.getCollection("test.coll");
        var bigstr = "a".repeat(4000);

        // Do not insert with a writeConcern because we want the delayed slave
        // to fall behind in replication. This is crucial apart from having a
        // readConcern to pin updates in memory on the primary. To prevent the
        // slave from falling off the oplog, we configure the oplog large enough
        // to accomodate all the inserts.
        for (var i = 0; i < 250; i++) {
            let batch = coll.initializeUnorderedBulkOp();
            for (var j = 0; j < 100; j++) {
                batch.insert({a: bigstr});
            }
            assert.writeOK(batch.execute());
        }
    }
})();
