/**
 * SERVER-31099 WiredTiger uses lookaside (LAS) file when the cache contents are
 * pinned and can not be evicted for example in the case of a delayed secondary
 * with read concern majority. This test inserts enough data where not using the
 * lookaside file results in a stall we can't recover from.
 *
 * This test is labeled resource intensive because its total io_write is 900MB.
 * @tags: [
 *   resource_intensive,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip db hash check because delayed secondary will not catch up to primary.
TestData.skipCheckDBHashes = true;

// Skip this test if not running with the "wiredTiger" storage engine.
let storageEngine = jsTest.options().storageEngine || "wiredTiger";
if (storageEngine !== "wiredTiger") {
    print('Skipping test because storageEngine is not "wiredTiger"');
    quit();
} else if (jsTest.options().wiredTigerCollectionConfigString === "type=lsm") {
    // Readers of old data, such as a lagged secondary, can lead to stalls when using
    // WiredTiger's LSM tree.
    print("WT-3742: Skipping test because we're running with WiredTiger's LSM tree");
    quit();
} else {
    let rst = new ReplSetTest({
        nodes: 2,
        // We are going to insert at least 100 MB of data with a long secondary
        // delay. Configure an appropriately large oplog size.
        oplogSize: 200,
    });

    let conf = rst.getReplSetConfig();
    conf.members[1].votes = 1;
    conf.members[1].priority = 0;

    rst.startSet();
    conf.members[1].secondaryDelaySecs = 24 * 60 * 60;

    // We cannot wait for a stable recovery timestamp, oplog replication, or config replication due
    // to the secondary delay.
    rst.initiate(conf, "replSetInitiate", {
        doNotWaitForStableRecoveryTimestamp: true,
        doNotWaitForReplication: true,
        doNotWaitForNewlyAddedRemovals: true,
    });
    let primary = rst.getPrimary(); // Waits for PRIMARY state.

    // The default WC is majority and we want the delayed secondary to fall behind in replication.
    // Retry to make sure the primary is done executing (but not necessarily replicating) the
    // reconfig.
    assert.soonNoExcept(function () {
        assert.commandWorked(
            primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: 1}}),
        );
        return true;
    });

    // Reconfigure primary with a small cache size so less data needs to be
    // inserted to make the cache full while trying to trigger a stall.
    assert.commandWorked(primary.adminCommand({setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=100MB"}));

    let coll = primary.getCollection("test.coll");
    let bigstr = "a".repeat(4000);

    // Do not insert with a writeConcern because we want the delayed secondary
    // to fall behind in replication. This is crucial apart from having a
    // readConcern to pin updates in memory on the primary. To prevent the
    // slave from falling off the oplog, we configure the oplog large enough
    // to accomodate all the inserts.
    for (let i = 0; i < 250; i++) {
        let batch = coll.initializeUnorderedBulkOp();
        for (let j = 0; j < 100; j++) {
            batch.insert({a: bigstr});
        }
        assert.commandWorked(batch.execute());
    }
    rst.stopSet();
}
