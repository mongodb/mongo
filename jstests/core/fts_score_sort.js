// Test sorting with text score metadata.
(function() {
    "use strict";

    var t = db.getSiblingDB("test").getCollection("fts_score_sort");
    t.drop();

    assert.writeOK(t.insert({_id: 0, a: "textual content"}));
    assert.writeOK(t.insert({_id: 1, a: "additional content"}));
    assert.writeOK(t.insert({_id: 2, a: "irrelevant content"}));
    assert.commandWorked(t.ensureIndex({a: "text"}));

    // $meta sort specification should be rejected if it has additional keys.
    assert.throws(function() {
        t.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
            .sort({score: {$meta: "textScore", extra: 1}})
            .itcount();
    });

    // $meta sort specification should be rejected if the type of meta sort is not known.
    assert.throws(function() {
        t.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
            .sort({score: {$meta: "unknown"}})
            .itcount();
    });

    // Sort spefication should be rejected if a $-keyword other than $meta is used.
    assert.throws(function() {
        t.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
            .sort({score: {$notMeta: "textScore"}})
            .itcount();
    });

    // Sort spefication should be rejected if it is a string, not an object with $meta.
    assert.throws(function() {
        t.find({$text: {$search: "textual content"}}, {score: {$meta: "textScore"}})
            .sort({score: "textScore"})
            .itcount();
    });

    // Sort by the text score.
    var results =
        t.find({$text: {$search: "textual content -irrelevant"}}, {score: {$meta: "textScore"}})
            .sort({score: {$meta: "textScore"}})
            .toArray();
    assert.eq(results.length, 2);
    assert.eq(results[0]._id, 0);
    assert.eq(results[1]._id, 1);
    assert.gt(results[0].score, results[1].score);

    // Sort by {_id descending, score} and verify the order is right.
    var results =
        t.find({$text: {$search: "textual content -irrelevant"}}, {score: {$meta: "textScore"}})
            .sort({_id: -1, score: {$meta: "textScore"}})
            .toArray();
    assert.eq(results.length, 2);
    assert.eq(results[0]._id, 1);
    assert.eq(results[1]._id, 0);
    // Note the reversal from above.
    assert.lt(results[0].score, results[1].score);
}());
