/**
 * Tests that wildcard indexes are prepared to handle and retry WriteConflictExceptions while
 * interacting with the storage layer to retrieve multikey paths.
 */
(function() {
    "strict";

    const conn = MongoRunner.runMongod();
    const testDB = conn.getDB("test");

    const coll = testDB.write_conflict_wildcard;
    coll.drop();

    assert.commandWorked(coll.createIndex({"$**": 1}));

    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: 'WTWriteConflictExceptionForReads',
        mode: {activationProbability: 0.01}
    }));
    for (let i = 0; i < 1000; ++i) {
        // Insert documents with a couple different multikey paths to increase the number of records
        // scanned during multikey path computation in the wildcard index.
        assert.commandWorked(coll.insert({
            _id: i,
            i: i,
            a: [{x: i - 1}, {x: i}, {x: i + 1}],
            b: [],
            longerName: [{nested: [1, 2]}, {nested: 4}]
        }));
        assert.eq(coll.find({i: i}).hint({"$**": 1}).itcount(), 1);
        if (i > 0) {
            assert.eq(coll.find({"a.x": i}).hint({"$**": 1}).itcount(), 2);
        }
    }

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'WTWriteConflictExceptionForReads', mode: "off"}));
    MongoRunner.stopMongod(conn);
})();
