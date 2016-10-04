// Test that the $bucketAuto stage defines and sorts buckets according to the collation.
(function() {
    "use strict";

    var results;
    const numericOrdering = {collation: {locale: "en_US", numericOrdering: true}};

    var coll = db.collation_bucket;
    coll.drop();

    function insertData() {
        assert.writeOK(coll.insert({num: "1"}));
        assert.writeOK(coll.insert({num: "2"}));
        assert.writeOK(coll.insert({num: "5"}));
        assert.writeOK(coll.insert({num: "10"}));
        assert.writeOK(coll.insert({num: "20"}));
        assert.writeOK(coll.insert({num: "50"}));
        assert.writeOK(coll.insert({num: "100"}));
        assert.writeOK(coll.insert({num: "200"}));
        assert.writeOK(coll.insert({num: "500"}));
    }

    insertData();

    // Test that $bucketAuto respects an explicit collation.
    results =
        coll.aggregate([{$bucketAuto: {groupBy: "$num", buckets: 3}}], numericOrdering).toArray();
    assert.eq(3, results.length);
    assert.eq({_id: {min: "1", max: "10"}, count: 3}, results[0]);
    assert.eq({_id: {min: "10", max: "100"}, count: 3}, results[1]);
    assert.eq({_id: {min: "100", max: "500"}, count: 3}, results[2]);

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    insertData();

    // Test that $bucketAuto respects the inherited collation.
    results = coll.aggregate([{$bucketAuto: {groupBy: "$num", buckets: 3}}]).toArray();
    assert.eq(3, results.length);
    assert.eq({_id: {min: "1", max: "10"}, count: 3}, results[0]);
    assert.eq({_id: {min: "10", max: "100"}, count: 3}, results[1]);
    assert.eq({_id: {min: "100", max: "500"}, count: 3}, results[2]);

    // Test that the collection default can be overridden with the simple collation. In this case,
    // the numbers will be sorted in lexicographical order, so the 3 buckets will be:
    // ["1", "10","100"], ["2", "20", "200"], and ["5", "50", "500"]
    results = coll.aggregate([{$bucketAuto: {groupBy: "$num", buckets: 3}}],
                             {collation: {locale: "simple"}})
                  .toArray();
    assert.eq(3, results.length);
    assert.eq({_id: {min: "1", max: "2"}, count: 3}, results[0]);
    assert.eq({_id: {min: "2", max: "5"}, count: 3}, results[1]);
    assert.eq({_id: {min: "5", max: "500"}, count: 3}, results[2]);
})();
