// Test that the $bucket stage defines and sorts buckets according to the collation.
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

    // Test that $bucket respects an explicit collation.
    results = coll.aggregate([{$bucket: {groupBy: "$num", boundaries: ["1", "10", "100", "1000"]}}],
                             numericOrdering)
                  .toArray();
    assert.eq(3, results.length);
    assert.eq({_id: "1", count: 3}, results[0]);
    assert.eq({_id: "10", count: 3}, results[1]);
    assert.eq({_id: "100", count: 3}, results[2]);

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
    insertData();

    // Test that $bucket respects the inherited collation.
    results = coll.aggregate([{$bucket: {groupBy: "$num", boundaries: ["1", "10", "100", "1000"]}}])
                  .toArray();
    assert.eq(3, results.length);
    assert.eq({_id: "1", count: 3}, results[0]);
    assert.eq({_id: "10", count: 3}, results[1]);
    assert.eq({_id: "100", count: 3}, results[2]);

    // Test that the collection default can be overridden with the simple collation. In this case,
    // the $bucket should fail, because under a lexicographical comparison strings like "2" or "5"
    // won't fall into any of the buckets.
    assert.throws(
        () => coll.aggregate([{$bucket: {groupBy: "$num", boundaries: ["1", "10", "100", "1000"]}}],
                             {collation: {locale: "simple"}}));
})();
