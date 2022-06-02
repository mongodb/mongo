/**
 * Tests that we can create both simple and compound geo indexes.
 * Also tests that a geo index can support non-geo searches on the indexed field.
 */
(function() {
'use strict';

const docs = [
    {_id: 1, y: [1, 1]},
    {_id: 2, y: [1, 1], z: 3},
    {_id: 3, y: [1, 1], z: 4},
    {_id: 4, y: [1, 1], z: 5},
];

let t = db.geo7_compound;
t.drop();

assert.commandWorked(t.createIndex({y: "2d", z: 1}));
assert.commandWorked(t.insert(docs));

assert.eq(1, t.find({y: [1, 1], z: 3}).itcount(), "A1");

t = db.geo7_simple;
t.drop();

assert.commandWorked(t.createIndex({y: "2d"}));
assert.commandWorked(t.insert(docs));

assert.eq(1, t.find({y: [1, 1], z: 3}).itcount(), "A2");

assert.commandWorked(t.insert({_id: 5, y: 5}));
assert.eq(5, t.findOne({y: 5})._id, "B1");
})();
