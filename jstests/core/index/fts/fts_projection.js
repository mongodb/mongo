// Test $text with $textScore projection.
// @tags: [
//   assumes_read_concern_local,
// ]

import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";

let t = db.getSiblingDB("test").getCollection("fts_projection");
t.drop();

assert.commandWorked(t.insert({_id: 0, a: "textual content"}));
assert.commandWorked(t.insert({_id: 1, a: "additional content", b: -1}));
assert.commandWorked(t.insert({_id: 2, a: "irrelevant content"}));
assert.commandWorked(t.createIndex({a: "text"}));

// Project the text score.
let results = t
    .find(
        {$text: {$search: "textual content -irrelevant"}},
        {
            score: {$meta: "textScore"},
        },
    )
    .toArray();
// printjson(results);
// Scores should exist.
assert.eq(results.length, 2);
assert(results[0].score);
assert(results[1].score);

// indexed by _id.
let scores = [0, 0, 0];
scores[results[0]._id] = results[0].score;
scores[results[1]._id] = results[1].score;

//
// Edge/error cases:
//

// Project text score into 3 fields, one nested.
results = t
    .find(
        {$text: {$search: "textual content -irrelevant"}},
        {
            otherScore: {$meta: "textScore"},
            score: {$meta: "textScore"},
            "nestedObj.score": {$meta: "textScore"},
        },
    )
    .toArray();
assert.eq(2, results.length);
for (var i = 0; i < results.length; ++i) {
    assert.close(scores[results[i]._id], results[i].score);
    assert.close(scores[results[i]._id], results[i].otherScore);
    assert.close(scores[results[i]._id], results[i].nestedObj.score);
}

// printjson(results);

// Project text score into "x.$" shouldn't crash
assert.throws(function () {
    t.find(
        {$text: {$search: "textual content -irrelevant"}},
        {
            "x.$": {$meta: "textScore"},
        },
    ).toArray();
});

// TODO: We can't project 'x.y':1 and 'x':1 (yet).

// Clobber an existing field and behave nicely.
results = t.find({$text: {$search: "textual content -irrelevant"}}, {b: {$meta: "textScore"}}).toArray();
assert.eq(2, results.length);
for (var i = 0; i < results.length; ++i) {
    assert.close(
        scores[results[i]._id],
        results[i].b,
        i + ": existing field in " + tojson(results[i], "", true) + " is not clobbered with score",
    );
}

assert.neq(-1, results[0].b);

// SERVER-12173
// When $text operator is in $or, should evaluate first
results = t
    .find(
        {$or: [{$text: {$search: "textual content -irrelevant"}}, {_id: 1}]},
        {
            score: {$meta: "textScore"},
        },
    )
    .toArray();
printjson(results);
assert.eq(2, results.length);
for (var i = 0; i < results.length; ++i) {
    assert.close(
        scores[results[i]._id],
        results[i].score,
        i + ": TEXT under OR invalid score: " + tojson(results[i], "", true),
    );
}

// SERVER-12592
// When $text operator is in $or, all non-$text children must be indexed. Otherwise, we should
// produce
// a readable error.
let errorMessage = "";
assert.throws(
    function () {
        try {
            t.find({$or: [{$text: {$search: "textual content -irrelevant"}}, {b: 1}]}).itcount();
        } catch (e) {
            errorMessage = e;
            throw e;
        }
    },
    [],
    "Expected error from failed TEXT under OR planning",
);
assert.neq(
    -1,
    errorMessage.message.indexOf("TEXT"),
    "message from failed text planning does not mention TEXT: " + errorMessage,
);
assert.neq(
    -1,
    errorMessage.message.indexOf("OR"),
    "message from failed text planning does not mention OR: " + errorMessage,
);

// SERVER-26833
// We should use the blocking "TEXT_OR" stage only if the projection calls for the "textScore"
// value.
let explainOutput = t
    .find(
        {$text: {$search: "textual content -irrelevant"}},
        {
            score: {$meta: "textScore"},
        },
    )
    .explain();
assert(planHasStage(db, getWinningPlanFromExplain(explainOutput), "TEXT_OR"), explainOutput);

explainOutput = t.find({$text: {$search: "textual content -irrelevant"}}).explain();
assert(!planHasStage(db, getWinningPlanFromExplain(explainOutput), "TEXT_OR"), explainOutput);

// Scores should exist.
assert.eq(results.length, 2);
assert(results[0].score, "invalid text score for " + tojson(results[0], "", true) + " when $text is in $or");
assert(results[1].score, "invalid text score for " + tojson(results[0], "", true) + " when $text is in $or");
