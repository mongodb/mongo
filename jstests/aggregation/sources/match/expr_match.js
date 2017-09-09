// Basic testing to confirm that the $match stage handles $expr correctly.
(function() {
    "use strict";

    const coll = db.expr_match;
    coll.drop();
    assert.writeOK(coll.insert({x: 0}));
    assert.writeOK(coll.insert({x: 1, y: 1}));
    assert.writeOK(coll.insert({x: 2, y: 4}));
    assert.writeOK(coll.insert({x: 3, y: 9}));

    // $match with $expr representing local document field path reference.
    assert.eq(1, coll.aggregate([{$match: {$expr: {$eq: ["$x", 2]}}}]).itcount());
    assert.eq(1, coll.aggregate([{$match: {$expr: {$eq: ["$x", "$y"]}}}]).itcount());
    assert.eq(3, coll.aggregate([{$match: {$expr: {$eq: ["$x", {$sqrt: "$y"}]}}}]).itcount());

    // $match with $expr containing $or and $and.
    assert.eq(
        2,
        coll.aggregate([{
                $match: {
                    $expr:
                        {$or: [{$eq: ["$x", 3]}, {$and: [{$eq: ["$x", 2]}, {$eq: ["$y", 4]}]}]}
                }
            }])
            .itcount());

    // $match $expr containing $in.
    assert.eq(3,
              coll.aggregate([{$match: {$expr: {$in: ["$x", [1, {$mod: [4, 2]}, 3]]}}}]).itcount());

    // $match with constant expression and field path.
    assert.eq(1,
              coll.aggregate([{$match: {$expr: {$gte: ["$y", {$multiply: [3, 3]}]}}}]).itcount());

    // $match with constant expression and no field path.
    assert.eq(4, coll.aggregate([{$match: {$expr: {$gte: [10, 5]}}}]).itcount());
    assert.eq(0, coll.aggregate([{$match: {$expr: {$gte: [5, 10]}}}]).itcount());
})();
