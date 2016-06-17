/**
 * Tests the behavior of how getMore operations are routed by the mongo shell when using a replica
 * set connection and cursors are established on a secondary.
 */
(function() {
    "use strict";

    var rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const dbName = "test";
    const collName = "getmore";

    // We create our own replica set connection because 'rst.nodes' is an array of direct
    // connections to each individual node.
    var conn = new Mongo(rst.getURL());

    // We force a read mode of "compatibility" so that we can test Mongo.prototype.readMode()
    // resolves to "legacy" independently of the --readMode passed to the mongo shell running this
    // test.
    conn.forceReadMode("compatibility");
    assert.eq("legacy",
              conn.readMode(),
              "replica set connections created by the mongo shell should use 'legacy' read mode");

    var coll = conn.getDB(dbName)[collName];
    coll.drop();

    // Insert several document so that we can use a cursor to fetch them in multiple batches.
    var res = coll.insert([{}, {}, {}, {}, {}]);
    assert.writeOK(res);
    assert.eq(5, res.nInserted);

    // Wait for the secondary to catch up because we're going to try and do reads from it.
    rst.awaitReplication();

    // Establish a cursor on the secondary and verify that the getMore operations are routed to it.
    conn.forceReadMode("compatibility");
    var cursor = coll.find().readPref("secondary").batchSize(2);
    assert.eq(5, cursor.itcount(), "failed to read the documents from the secondary");

    rst.stopSet();
})();
