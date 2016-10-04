// Test that duplicate query results are not returned.

var t = db.jstests_find_dedup;

function checkDedup(query, idArray) {
    resultsArr = t.find(query).toArray();
    assert.eq(resultsArr.length, idArray.length, "same number of results");

    for (var i = 0; i < idArray.length; i++) {
        assert(("_id" in resultsArr[i]), "result doc missing _id");
        assert.eq(idArray[i], resultsArr[i]._id, "_id mismatch for doc " + i);
    }
}

// Deduping $or
t.drop();
t.ensureIndex({a: 1, b: 1});
t.save({_id: 1, a: 1, b: 1});
t.save({_id: 2, a: 1, b: 1});
t.save({_id: 3, a: 2, b: 2});
t.save({_id: 4, a: 3, b: 3});
t.save({_id: 5, a: 3, b: 3});
checkDedup({
    $or: [
        {a: {$gte: 0, $lte: 2}, b: {$gte: 0, $lte: 2}},
        {a: {$gte: 1, $lte: 3}, b: {$gte: 1, $lte: 3}},
        {a: {$gte: 1, $lte: 4}, b: {$gte: 1, $lte: 4}}
    ]
},
           [1, 2, 3, 4, 5]);

// Deduping multikey
t.drop();
t.save({_id: 1, a: [1, 2, 3], b: [4, 5, 6]});
t.save({_id: 2, a: [1, 2, 3], b: [4, 5, 6]});
assert.eq(2, t.count());
checkDedup({$or: [{a: {$in: [1, 2]}}, {b: {$in: [4, 5]}}]}, [1, 2]);
t.ensureIndex({a: 1});
checkDedup({$or: [{a: {$in: [1, 2]}}, {b: {$in: [4, 5]}}]}, [1, 2]);
