/**
 * Tests that a user can group on the text score.
 */
const coll = db.text_score_grouping;

coll.drop();

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));

assert.commandWorked(coll.createIndex({title: "text"}));

// Make sure there are two distinct groups for a text search with no other dependencies.
let results = coll
    .aggregate([{$match: {$text: {$search: "cake cookies"}}}, {$group: {_id: {$meta: "textScore"}, count: {$sum: 1}}}])
    .toArray();
assert.eq(results.length, 2);

// Make sure there are two distinct groups if there are other fields required by the group.
results = coll
    .aggregate([
        {$match: {$text: {$search: "cake cookies"}}},
        {$group: {_id: {$meta: "textScore"}, firstId: {$first: "$_id"}}},
    ])
    .toArray();
assert.eq(results.length, 2);
