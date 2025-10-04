// Cannot implicitly shard accessed collections because of collection existing when none expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
// ]

// Test that the $match stage respects the collation.
let caseInsensitive = {collation: {locale: "en_US", strength: 2}};

let coll = db.collation_match;
coll.drop();
assert.commandWorked(coll.insert({a: "a"}));

// Test that the $match respects an explicit collation when it can be pushed down into the query
// layer.
assert.eq(1, coll.aggregate([{$match: {a: "A"}}], caseInsensitive).itcount());

// Test that the $match respects an explicit collation when it cannot be pushed down into the
// query layer.
assert.eq(1, coll.aggregate([{$project: {b: "B"}}, {$match: {b: "b"}}], caseInsensitive).itcount());

// Test that $match inside a $facet respects the collation.
assert.eq(1, coll.aggregate([{$facet: {fct: [{$match: {a: "A"}}]}}], caseInsensitive).toArray()[0].fct.length);

// Test that when a $match can be split to be part before the $unwind and part after, both
// pieces of the split respect the collation.
coll.drop();
assert.commandWorked(coll.insert({a: "foo", b: ["bar"]}));
assert.eq(1, coll.aggregate([{$limit: 1}, {$unwind: "$b"}, {$match: {a: "FOO", b: "BAR"}}], caseInsensitive).itcount());

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), caseInsensitive));
assert.commandWorked(coll.insert({a: "a"}));

// Test that the $match respects the inherited collation when it can be pushed down into the
// query layer.
assert.eq(1, coll.aggregate([{$match: {a: "A"}}]).itcount());

// Test that the $match respects the inherited collation when it cannot be pushed down into the
// query layer.
assert.eq(1, coll.aggregate([{$project: {b: "B"}}, {$match: {b: "b"}}]).itcount());
