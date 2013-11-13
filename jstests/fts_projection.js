// Test $text with $textScore projection.

var t = db.getSiblingDB("test").getCollection("fts_projection");
t.drop();

db.adminCommand({setParameter:1, textSearchEnabled:true});
db.adminCommand({setParameter:1, newQueryFrameworkEnabled:true});

t.insert({_id:0, _idCopy: 0, a:"textual content"});
t.insert({_id:1, _idCopy: 1, a:"additional content"});
t.insert({_id:2, _idCopy: 2, a:"irrelevant content"});
t.ensureIndex({a:"text"});

// Project the text score.
var results = t.find({$text: {$search: "textual content -irrelevant"}}, {score:{$textScore: 1}}).toArray();
printjson(results);
assert.eq(results.length, 2);
assert.eq(results[0]._id, 0);
assert.eq(results[1]._id, 1);
assert(results[0].score > results[1].score);

// Don't crash if we have no text score.
var results = t.find({a: /text/}, {score:{$textScore: 1}}).toArray();
printjson(results);
