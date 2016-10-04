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

    // Test that $bucket rejects boundaries that are not sorted according to the collation.
    assert.throws(
        () => coll.aggregate([{$bucket: {groupBy: "$num", boundaries: ["100", "20", "4"]}}]));

    assert.throws(() =>
                      coll.aggregate([{$bucket: {groupBy: "$num", boundaries: ["4", "20", "100"]}}],
                                     {collation: {locale: "simple"}}));

    // Test that $bucket rejects a default value that falls within the boundaries.
    assert.throws(
        () => coll.aggregate(
            [{$bucket: {groupBy: "$num", boundaries: ["1", "10", "100"], default: "40"}}]));

    assert.throws(() => coll.aggregate(
                      [{$bucket: {groupBy: "$num", boundaries: ["100", "999"], default: "2"}}],
                      {collation: {locale: "simple"}}));

    // Test that $bucket accepts a default value that falls outside the boundaries according to the
    // collation.
    results =
        coll.aggregate([{
                $bucket: {
                    groupBy: "$num",
                    boundaries: ["100", "999"],
                    default: "2"  // Would fall between 100 and 999 if using the simple collation.
                }
            }])
            .toArray();
    assert.eq(2, results.length);
    assert.eq({_id: "2", count: 6}, results[0]);
    assert.eq({_id: "100", count: 3}, results[1]);  // "100", "200", and "500".

    results =
        coll.aggregate(
                [{
                   $bucket: {
                       groupBy: "$num",
                       boundaries: ["1", "19999"],  // Will include all numbers that start with "1"
                       default: "2"                 // Would fall between boundaries if using the
                                                    // collection-default collation with numeric
                                                    // ordering.
                   }
                }],
                {collation: {locale: "simple"}})
            .toArray();
    assert.eq(2, results.length);
    assert.eq({_id: "1", count: 3}, results[0]);  // "1", "10", and "100".
    assert.eq({_id: "2", count: 6}, results[1]);
})();
