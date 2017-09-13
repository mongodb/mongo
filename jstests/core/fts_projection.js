// Test $text with $textScore projection.

var t = db.getSiblingDB("test").getCollection("fts_projection");
t.drop();

t.insert({_id: 0, a: "textual content"});
t.insert({_id: 1, a: "additional content", b: -1});
t.insert({_id: 2, a: "irrelevant content"});
t.ensureIndex({a: "text"});

// Project the text score.
var results = t.find({$text: {$search: "textual content -irrelevant"}}, {
                   _idCopy: 0,
                   score: {$meta: "textScore"}
               }).toArray();
// printjson(results);
// Scores should exist.
assert.eq(results.length, 2);
assert(results[0].score);
assert(results[1].score);

// indexed by _id.
var scores = [0, 0, 0];
scores[results[0]._id] = results[0].score;
scores[results[1]._id] = results[1].score;

//
// Edge/error cases:
//

// Project text score into 2 fields.
results = t.find({$text: {$search: "textual content -irrelevant"}}, {
               otherScore: {$meta: "textScore"},
               score: {$meta: "textScore"}
           }).toArray();
assert.eq(2, results.length);
for (var i = 0; i < results.length; ++i) {
    assert.close(scores[results[i]._id], results[i].score);
    assert.close(scores[results[i]._id], results[i].otherScore);
}

// printjson(results);

// Project text score into "x.$" shouldn't crash
assert.throws(function() {
    t.find({$text: {$search: "textual content -irrelevant"}}, {
         'x.$': {$meta: "textScore"}
     }).toArray();
});

// TODO: We can't project 'x.y':1 and 'x':1 (yet).

// Clobber an existing field and behave nicely.
results =
    t.find({$text: {$search: "textual content -irrelevant"}}, {b: {$meta: "textScore"}}).toArray();
assert.eq(2, results.length);
for (var i = 0; i < results.length; ++i) {
    assert.close(
        scores[results[i]._id],
        results[i].b,
        i + ': existing field in ' + tojson(results[i], '', true) + ' is not clobbered with score');
}

assert.neq(-1, results[0].b);

// Don't crash if we have no text score.
var results = t.find({a: /text/}, {score: {$meta: "textScore"}}).toArray();
// printjson(results);

// No textScore proj. with nested fields
assert.throws(function() {
    t.find({$text: {$search: "blah"}}, {'x.y': {$meta: "textScore"}}).toArray();
});

// SERVER-12173
// When $text operator is in $or, should evaluate first
results = t.find({$or: [{$text: {$search: "textual content -irrelevant"}}, {_id: 1}]}, {
               _idCopy: 0,
               score: {$meta: "textScore"}
           }).toArray();
printjson(results);
assert.eq(2, results.length);
for (var i = 0; i < results.length; ++i) {
    assert.close(scores[results[i]._id],
                 results[i].score,
                 i + ': TEXT under OR invalid score: ' + tojson(results[i], '', true));
}

// SERVER-12592
// When $text operator is in $or, all non-$text children must be indexed. Otherwise, we should
// produce
// a readable error.
var errorMessage = '';
assert.throws(function() {
    try {
        t.find({$or: [{$text: {$search: "textual content -irrelevant"}}, {b: 1}]}).itcount();
    } catch (e) {
        errorMessage = e;
        throw e;
    }
}, [], 'Expected error from failed TEXT under OR planning');
assert.neq(-1,
           errorMessage.message.indexOf('TEXT'),
           'message from failed text planning does not mention TEXT: ' + errorMessage);
assert.neq(-1,
           errorMessage.message.indexOf('OR'),
           'message from failed text planning does not mention OR: ' + errorMessage);

// Scores should exist.
assert.eq(results.length, 2);
assert(results[0].score,
       "invalid text score for " + tojson(results[0], '', true) + " when $text is in $or");
assert(results[1].score,
       "invalid text score for " + tojson(results[0], '', true) + " when $text is in $or");
