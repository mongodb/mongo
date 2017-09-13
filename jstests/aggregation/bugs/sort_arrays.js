// Tests that sorting by a field that contains an array will sort by the minimum element in that
// array.
(function() {
    "use strict";
    const coll = db.foo;
    coll.drop();
    assert.writeOK(coll.insert([{_id: 2, a: [2, 3]}, {_id: 3, a: [2, 4]}, {_id: 4, a: [2, 1]}]));
    const expectedOrder = [{_id: 4, a: [2, 1]}, {_id: 2, a: [2, 3]}, {_id: 3, a: [2, 4]}];

    assert.eq(coll.aggregate([{$sort: {a: 1}}]).toArray(), expectedOrder);
    assert.eq(coll.find().sort({a: 1}).toArray(), expectedOrder);

    assert.commandWorked(coll.ensureIndex({a: 1}));
    assert.eq(coll.aggregate([{$sort: {a: 1}}]).toArray(), expectedOrder);
    assert.eq(coll.find().sort({a: 1}).toArray(), expectedOrder);
}());
