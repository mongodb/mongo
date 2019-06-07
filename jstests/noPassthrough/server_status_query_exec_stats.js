/**
 * Tests for serverStatus metrics.queryExecutor stats.
 */
(function() {
    "use strict";

    if (jsTest.options().storageEngine === "mobile") {
        print("Skipping test because storage engine isn't mobile");
        return;
    }

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn, "mongod was unable to start up");
    const db = conn.getDB(jsTest.name());
    const coll = db[jsTest.name()];

    let getCollectionScans = () => {
        return db.serverStatus().metrics.queryExecutor.collectionScans.total;
    };
    let getCollectionScansNonTailable = () => {
        return db.serverStatus().metrics.queryExecutor.collectionScans.nonTailable;
    };

    // Create and populate a capped collection so that we can run tailable queries.
    const nDocs = 32;
    coll.drop();
    assert.commandWorked(db.createCollection(jsTest.name(), {capped: true, size: nDocs * 100}));

    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }

    // Test nontailable collection scans update collectionScans counters appropriately.
    for (let i = 0; i < nDocs; i++) {
        assert.eq(coll.find({a: i}).itcount(), 1);
        assert.eq(i + 1, getCollectionScans());
        assert.eq(i + 1, getCollectionScansNonTailable());
    }

    // Test tailable collection scans update collectionScans counters appropriately.
    for (let i = 0; i < nDocs; i++) {
        assert.eq(coll.find({a: i}).tailable().itcount(), 1);
        assert.eq(nDocs + i + 1, getCollectionScans());
        assert.eq(nDocs, getCollectionScansNonTailable());
    }

    // Run a query which will require the client to fetch multiple batches from the server. Ensure
    // that the getMore commands don't increment the counter of collection scans.
    assert.eq(coll.find({}).batchSize(2).itcount(), nDocs);
    assert.eq((nDocs * 2) + 1, getCollectionScans());
    assert.eq(nDocs + 1, getCollectionScansNonTailable());

    // Create index to test that index scans don't up the collection scan counter.
    assert.commandWorked(coll.createIndex({a: 1}));
    // Run a bunch of index scans.
    for (let i = 0; i < nDocs; i++) {
        assert.eq(coll.find({a: i}).itcount(), 1);
    }
    // Assert that the number of collection scans hasn't increased.
    assert.eq((nDocs * 2) + 1, getCollectionScans());
    assert.eq(nDocs + 1, getCollectionScansNonTailable());

    MongoRunner.stopMongod(conn);
}());
