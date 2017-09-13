// Test that a query with default read preference ("primary") will succeed even if the node being
// queried steps down before the final result batch has been delivered.
(function() {
    "use strict";
    var dbName = "test";
    var collName = jsTest.name();

    function runTest(host, rst, waitForPrimary) {
        // We create a new connection to 'host' here instead of passing in the original connection.
        // This to work around the fact that connections created by ReplSetTest already have slaveOk
        // set on them, but we need a connection with slaveOk not set for this test.
        var conn = new Mongo(host);
        var coll = conn.getDB(dbName).getCollection(collName);
        assert(!coll.exists());
        assert.writeOK(coll.insert([{}, {}, {}, {}, {}]));
        var cursor = coll.find().batchSize(2);
        // Retrieve the first batch of results.
        cursor.next();
        cursor.next();
        assert.eq(0, cursor.objsLeftInBatch());
        var primary = rst.getPrimary();
        var secondary = rst.getSecondary();
        assert.throws(function() {
            primary.getDB("admin").runCommand({replSetStepDown: 60, force: true});
        });
        rst.waitForState(primary, ReplSetTest.State.SECONDARY);
        if (waitForPrimary) {
            rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
        }
        // When the primary steps down, it closes all client connections. Since 'conn' may be a
        // direct connection to the primary and the shell doesn't automatically retry operations on
        // network errors, we run a dummy operation here to force the shell to reconnect.
        try {
            conn.getDB("admin").runCommand("ping");
        } catch (e) {
        }

        // Even though our connection doesn't have slaveOk set, we should still be able to iterate
        // our cursor and kill our cursor.
        assert(cursor.hasNext());
        assert.doesNotThrow(function() {
            cursor.close();
        });
    }

    // Test querying a replica set primary directly.
    var rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    runTest(rst.getPrimary().host, rst, false);
    rst.stopSet();

    rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    runTest(rst.getURL(), rst, true);
    rst.stopSet();

    // Test querying a replica set primary through mongos.
    var st = new ShardingTest({shards: 1, rs: true});
    rst = st.rs0;
    runTest(st.s0.host, rst);
    st.stop();
})();
