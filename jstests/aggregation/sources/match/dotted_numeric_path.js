/**
 * Tests that $match works correctly with dotted numeric path.
 */
(function() {
"use strict";

const collName = "dotted_numeric_path";
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insert({"_id": 1, "quizzes": [{"score": 100}]}));
assert.commandWorked(coll.insert({"_id": 2, "quizzes": [{"score": 200}]}));

const res = coll.aggregate([{$match: {'quizzes.0.score': {$gt: 0}}}, {$count: 'count'}]).toArray();

assert.eq(res.length, 1);
assert.eq(res[0]['count'], 2);
}());
