// Test sorting with text score metadata.

var t = db.getSiblingDB("test").getCollection("fts_score_sort");
t.drop();

db.adminCommand({setParameter: 1, newQueryFrameworkEnabled: true});

t.insert({_id: 0, a: "textual content"});
t.insert({_id: 1, a: "additional content"});
t.insert({_id: 2, a: "irrelevant content"});
t.ensureIndex({a: "text"});

// Sort by the text score.
var results =
    t.find({$text: {$search: "textual content -irrelevant"}}, {score: {$meta: "textScore"}})
        .sort({score: {$meta: "textScore"}})
        .toArray();
// printjson(results);
assert.eq(results.length, 2);
assert.eq(results[0]._id, 0);
assert.eq(results[1]._id, 1);
assert(results[0].score > results[1].score);

// Sort by {_id descending, score} and verify the order is right.
var results =
    t.find({$text: {$search: "textual content -irrelevant"}}, {score: {$meta: "textScore"}})
        .sort({_id: -1, score: {$meta: "textScore"}})
        .toArray();
printjson(results);
assert.eq(results.length, 2);
assert.eq(results[0]._id, 1);
assert.eq(results[1]._id, 0);
// Note the reversal from above.
assert(results[0].score < results[1].score);
