// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

// Integration tests for collation-aware updates.
const coll = db[jsTestName()];

const caseInsensitive = {
    collation: {locale: "en_US", strength: 2},
};
const caseSensitive = {
    collation: {locale: "en_US", strength: 3},
};
const numericOrdering = {
    collation: {locale: "en_US", numericOrdering: true},
};

// Update modifiers respect collection default collation on simple _id query.
coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
assert.commandWorked(coll.insert({_id: 1, a: "124"}));
assert.commandWorked(coll.update({_id: 1}, {$min: {a: "1234"}}));
assert.eq(coll.find({a: "124"}).count(), 1);

// Simple _id query with hint on different index should work.
assert.commandWorked(coll.createIndex({foobar: 1}));
function makeUpdateCmdWithHint(update, hint) {
    return {update: coll.getName(), updates: [{q: {_id: 1}, u: update, hint: hint}]};
}
assert.commandWorked(coll.runCommand(makeUpdateCmdWithHint({$min: {a: "49"}}, {foobar: 1})));
assert.eq(coll.find({a: "49"}).count(), 1);
assert.commandWorked(coll.runCommand(makeUpdateCmdWithHint({$min: {a: "5"}}, {_id: 1})));
assert.eq(coll.find({a: "5"}).count(), 1);

// $min respects query collation.
assert(coll.drop());

// 1234 > 124, so no change should occur.
assert.commandWorked(coll.insert({a: "124"}));
assert.commandWorked(coll.update({a: "124"}, {$min: {a: "1234"}}, numericOrdering));
assert.eq(coll.find({a: "124"}).count(), 1);

// "1234" < "124" (non-numeric ordering), so an update should occur.
assert.commandWorked(coll.update({a: "124"}, {$min: {a: "1234"}}, caseSensitive));
assert.eq(coll.find({a: "1234"}).count(), 1);

// $min respects collection default collation.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
assert.commandWorked(coll.insert({a: "124"}));
assert.commandWorked(coll.update({a: "124"}, {$min: {a: "1234"}}));
assert.eq(coll.find({a: "124"}).count(), 1);

// $max respects query collation.
assert(coll.drop());

// "1234" < "124", so an update should not occur.
assert.commandWorked(coll.insert({a: "124"}));
assert.commandWorked(coll.update({a: "124"}, {$max: {a: "1234"}}, caseSensitive));
assert.eq(coll.find({a: "124"}).count(), 1);

// 1234 > 124, so an update should occur.
assert.commandWorked(coll.update({a: "124"}, {$max: {a: "1234"}}, numericOrdering));
assert.eq(coll.find({a: "1234"}).count(), 1);

// $max respects collection default collation.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
assert.commandWorked(coll.insert({a: "124"}));
assert.commandWorked(coll.update({a: "124"}, {$max: {a: "1234"}}));
assert.eq(coll.find({a: "1234"}).count(), 1);

// $addToSet respects query collation.
assert(coll.drop());

// "foo" == "FOO" (case-insensitive), so set isn't extended.
assert.commandWorked(coll.insert({a: ["foo"]}));
assert.commandWorked(coll.update({}, {$addToSet: {a: "FOO"}}, caseInsensitive));
var set = coll.findOne().a;
assert.eq(set.length, 1);

// "foo" != "FOO" (case-sensitive), so set is extended.
assert.commandWorked(coll.update({}, {$addToSet: {a: "FOO"}}, caseSensitive));
set = coll.findOne().a;
assert.eq(set.length, 2);

assert(coll.drop());

// $each and $addToSet respect collation
assert.commandWorked(coll.insert({a: ["foo", "bar", "FOO"]}));
assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: ["FOO", "BAR", "str"]}}}, caseInsensitive));
set = coll.findOne().a;
assert.eq(set.length, 4);
assert(set.includes("foo"));
assert(set.includes("FOO"));
assert(set.includes("bar"));
assert(set.includes("str"));

assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
// "foo" == "FOO" (case-insensitive), so set isn't extended.
assert.commandWorked(coll.insert({a: ["foo"]}));
assert.commandWorked(coll.update({}, {$addToSet: {a: "FOO"}}));
var set = coll.findOne().a;
assert.eq(set.length, 1);

// $pull respects query collation.
assert(coll.drop());

// "foo" != "FOO" (case-sensitive), so it is not pulled.
assert.commandWorked(coll.insert({a: ["foo", "FOO"]}));
assert.commandWorked(coll.update({}, {$pull: {a: "foo"}}, caseSensitive));
var arr = coll.findOne().a;
assert.eq(arr.length, 1);
assert(arr.includes("FOO"));

// "foo" == "FOO" (case-insensitive), so "FOO" is pulled.
assert.commandWorked(coll.update({}, {$pull: {a: "foo"}}, caseInsensitive));
arr = coll.findOne().a;
assert.eq(arr.length, 0);

// collation-aware $pull removes all instances that match.
assert(coll.drop());
assert.commandWorked(coll.insert({a: ["foo", "FOO"]}));
assert.commandWorked(coll.update({}, {$pull: {a: "foo"}}, caseInsensitive));
arr = coll.findOne().a;
assert.eq(arr.length, 0);

// collation-aware $pull with comparisons removes matching instances.
assert(coll.drop());

// "124" > "1234" (case-sensitive), so it is not removed.
assert.commandWorked(coll.insert({a: ["124", "1234"]}));
assert.commandWorked(coll.update({}, {$pull: {a: {$lt: "1234"}}}, caseSensitive));
arr = coll.findOne().a;
assert.eq(arr.length, 2);

// 124 < 1234 (numeric ordering), so it is removed.
assert.commandWorked(coll.update({}, {$pull: {a: {$lt: "1234"}}}, numericOrdering));
arr = coll.findOne().a;
assert.eq(arr.length, 1);
assert(arr.includes("1234"));

// $pull respects collection default collation.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({a: ["foo", "FOO"]}));
assert.commandWorked(coll.update({}, {$pull: {a: "foo"}}));
var arr = coll.findOne().a;
assert.eq(arr.length, 0);

// $pullAll respects query collation.
assert(coll.drop());

// "foo" != "FOO" (case-sensitive), so no changes are made.
assert.commandWorked(coll.insert({a: ["foo", "bar"]}));
assert.commandWorked(coll.update({}, {$pullAll: {a: ["FOO", "BAR"]}}, caseSensitive));
var arr = coll.findOne().a;
assert.eq(arr.length, 2);

// "foo" == "FOO", "bar" == "BAR" (case-insensitive), so both are removed.
assert.commandWorked(coll.update({}, {$pullAll: {a: ["FOO", "BAR"]}}, caseInsensitive));
arr = coll.findOne().a;
assert.eq(arr.length, 0);

// $pullAll respects collection default collation.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({a: ["foo", "bar"]}));
assert.commandWorked(coll.update({}, {$pullAll: {a: ["FOO", "BAR"]}}));
var arr = coll.findOne().a;
assert.eq(arr.length, 0);

// $push with $sort respects query collation.
assert(coll.drop());

// "1230" < "1234" < "124" (case-sensitive)
assert.commandWorked(coll.insert({a: ["1234", "124"]}));
assert.commandWorked(coll.update({}, {$push: {a: {$each: ["1230"], $sort: 1}}}, caseSensitive));
var arr = coll.findOne().a;
assert.eq(arr.length, 3);
assert.eq(arr[0], "1230");
assert.eq(arr[1], "1234");
assert.eq(arr[2], "124");

// "124" < "1230" < "1234" (numeric ordering)
assert(coll.drop());
assert.commandWorked(coll.insert({a: ["1234", "124"]}));
assert.commandWorked(coll.update({}, {$push: {a: {$each: ["1230"], $sort: 1}}}, numericOrdering));
arr = coll.findOne().a;
assert.eq(arr.length, 3);
assert.eq(arr[0], "124");
assert.eq(arr[1], "1230");
assert.eq(arr[2], "1234");

// $push with $sort respects collection default collation.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));
assert.commandWorked(coll.insert({a: ["1234", "124"]}));
assert.commandWorked(coll.update({}, {$push: {a: {$each: ["1230"], $sort: 1}}}));
var arr = coll.findOne().a;
assert.eq(arr.length, 3);
assert.eq(arr[0], "124");
assert.eq(arr[1], "1230");
assert.eq(arr[2], "1234");

// $ positional operator respects query collation on $set.
assert(coll.drop());

// "foo" != "FOO" (case-sensitive) so no update occurs.
assert.commandWorked(coll.insert({a: ["foo", "FOO"]}));
assert.commandWorked(coll.update({a: "FOO"}, {$set: {"a.$": "FOO"}}, caseSensitive));
var arr = coll.findOne().a;
assert.eq(arr.length, 2);
assert.eq(arr[0], "foo");
assert.eq(arr[1], "FOO");

// "foo" == "FOO" (case-insensitive) so an update occurs.
assert.commandWorked(coll.update({a: "FOO"}, {$set: {"a.$": "FOO"}}, caseInsensitive));
var arr = coll.findOne().a;
assert.eq(arr.length, 2);
assert.eq(arr[0], "FOO");
assert.eq(arr[1], "FOO");

// $ positional operator respects collection default collation on $set.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({a: ["foo", "FOO"]}));
assert.commandWorked(coll.update({a: "FOO"}, {$set: {"a.$": "FOO"}}));
var arr = coll.findOne().a;
assert.eq(arr.length, 2);
assert.eq(arr[0], "FOO");
assert.eq(arr[1], "FOO");

// Pipeline-style update respects collection default collation.
assert(coll.drop());
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}]));
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);

// Pipeline-style update respects query collation.
//
// Case sensitive $indexOfArray on "B" matches "B".
assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}], caseSensitive));
assert.eq(coll.findOne().newField, 5, `actual=${coll.findOne()}`);

assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$project: {newField: {$indexOfArray: ["$x", "B"]}}}], caseSensitive));
assert.eq(coll.findOne().newField, 5, `actual=${coll.findOne()}`);

assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$replaceWith: {newField: {$indexOfArray: ["$x", "B"]}}}], caseSensitive));
assert.eq(coll.findOne().newField, 5, `actual=${coll.findOne()}`);

// Case insensitive $indexOfArray on "B" matches "b".
assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}], caseInsensitive));
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);

assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$project: {newField: {$indexOfArray: ["$x", "B"]}}}], caseInsensitive));
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);

assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(coll.update({}, [{$replaceWith: {newField: {$indexOfArray: ["$x", "B"]}}}], caseInsensitive));
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);

// Collation is respected for pipeline-style bulk update.
assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(
    coll.bulkWrite([
        {
            updateOne: {
                filter: {},
                update: [{$addFields: {newField: {$indexOfArray: ["$x", "B"]}}}],
                collation: caseInsensitive.collation,
            },
        },
    ]),
);
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);

assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(
    coll.bulkWrite([
        {
            updateOne: {
                filter: {},
                update: [{$project: {newField: {$indexOfArray: ["$x", "B"]}}}],
                collation: caseInsensitive.collation,
            },
        },
    ]),
);
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);

assert(coll.drop());
assert.commandWorked(coll.insert({x: [1, 2, "a", "b", "c", "B"]}));
assert.commandWorked(
    coll.bulkWrite([
        {
            updateOne: {
                filter: {},
                update: [{$replaceWith: {newField: {$indexOfArray: ["$x", "B"]}}}],
                collation: caseInsensitive.collation,
            },
        },
    ]),
);
assert.eq(coll.findOne().newField, 3, `actual=${coll.findOne()}`);
