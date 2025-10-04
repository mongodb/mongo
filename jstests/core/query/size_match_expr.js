// Tests the behavior of $size for match expressions.
// @tags: [
//   requires_getmore,
// ]

const coll = db[jsTestName()];
coll.drop();

// Test $size when it's nested inside $and/$or.
assert.commandWorked(
    coll.insert([
        {a: 1, b: "foo"},
        {a: 1, b: [7, 8, 9]},
    ]),
);
assert.eq(1, coll.find({$or: [{$and: [{b: {$size: 3}}, {a: 1}]}, {a: 0}]}).itcount());
