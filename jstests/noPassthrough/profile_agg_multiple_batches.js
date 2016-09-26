// Tests that keysExamined and docsExamined are correct for aggregation when multiple batches pass
// through DocumentSourceCursor.

(function() {
    "use strict";

    load("jstests/libs/profiler.js");

    // Setting internalDocumentSourceCursorBatchSizeBytes=1 ensures that multiple batches pass
    // through DocumentSourceCursor.
    const options = {setParameter: "internalDocumentSourceCursorBatchSizeBytes=1"};
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

    const testDB = conn.getDB("test");
    const coll = testDB.getCollection("coll");

    testDB.setProfilingLevel(2);

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i, b: i}));
    }

    assert.commandWorked(coll.createIndex({a: 1}));

    assert.eq(8, coll.aggregate([{$match: {a: {$gte: 2}}}, {$group: {_id: "$b"}}]).itcount());

    const profileObj = getLatestProfilerEntry(testDB);

    assert.eq(profileObj.keysExamined, 8, tojson(profileObj));
    assert.eq(profileObj.docsExamined, 8, tojson(profileObj));

    MongoRunner.stopMongod(conn);
})();
