// Test that duplicate query results are not returned.
(function() {
    "use strict";

    const coll = db.jstests_find_dedup;

    function checkDedup(query, idArray) {
        const resultsArr = coll.find(query).sort({_id: 1}).toArray();
        assert.eq(resultsArr.length, idArray.length, "same number of results");

        for (let i = 0; i < idArray.length; i++) {
            assert(("_id" in resultsArr[i]), "result doc missing _id");
            assert.eq(idArray[i], resultsArr[i]._id, "_id mismatch for doc " + i);
        }
    }

    // Deduping $or
    coll.drop();
    coll.ensureIndex({a: 1, b: 1});
    assert.writeOK(coll.insert({_id: 1, a: 1, b: 1}));
    assert.writeOK(coll.insert({_id: 2, a: 1, b: 1}));
    assert.writeOK(coll.insert({_id: 3, a: 2, b: 2}));
    assert.writeOK(coll.insert({_id: 4, a: 3, b: 3}));
    assert.writeOK(coll.insert({_id: 5, a: 3, b: 3}));
    checkDedup({
        $or: [
            {a: {$gte: 0, $lte: 2}, b: {$gte: 0, $lte: 2}},
            {a: {$gte: 1, $lte: 3}, b: {$gte: 1, $lte: 3}},
            {a: {$gte: 1, $lte: 4}, b: {$gte: 1, $lte: 4}}
        ]
    },
               [1, 2, 3, 4, 5]);

    // Deduping multikey
    assert(coll.drop());
    assert.writeOK(coll.insert({_id: 1, a: [1, 2, 3], b: [4, 5, 6]}));
    assert.writeOK(coll.insert({_id: 2, a: [1, 2, 3], b: [4, 5, 6]}));
    assert.eq(2, coll.count());

    checkDedup({$or: [{a: {$in: [1, 2]}}, {b: {$in: [4, 5]}}]}, [1, 2]);

    assert.commandWorked(coll.createIndex({a: 1}));
    checkDedup({$or: [{a: {$in: [1, 2]}}, {b: {$in: [4, 5]}}]}, [1, 2]);
}());
