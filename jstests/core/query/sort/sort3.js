// @tags: [
//   requires_getmore,
// ]

const coll = db.sort3;
coll.drop();

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 5}));
assert.commandWorked(coll.insert({a: 3}));

assert.eq(
    [1, 3, 5],
    coll
        .find()
        .sort({a: 1})
        .toArray()
        .map((doc) => doc.a),
);
assert.eq(
    [5, 3, 1],
    coll
        .find()
        .sort({a: -1})
        .toArray()
        .map((doc) => doc.a),
);
