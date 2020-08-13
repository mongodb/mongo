(function() {
"use strict";
const coll = db.index_dates;
coll.drop();

// Min value for JS Date().
// @tags: [
//   sbe_incompatible,
// ]
const d1 = new Date(-8640000000000000);
assert.commandWorked(coll.insert({_id: 1, d: d1}));
// Max value for JS Date().
const d2 = new Date(8640000000000000);
assert.commandWorked(coll.insert({_id: 2, d: d2}));

assert.commandWorked(coll.insert({_id: 3, d: 100}));

function test() {
    const list = coll.find({d: {$type: "date"}}).sort({_id: 1}).toArray();
    assert.eq(2, list.length);
    assert.eq(list[0], {_id: 1, d: d1});
    assert.eq(list[1], {_id: 2, d: d2});
}

test();
// Testing index version 1.
assert.commandWorked(coll.createIndex({d: 1}, {v: 1}));
test();
assert.commandWorked(coll.dropIndex({d: 1}));
// Testing index version 2.
assert.commandWorked(coll.createIndex({d: 1}));
test();
})();