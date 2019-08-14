// server 6779: serializing ExpressionCoerceToBool
// This test only fails in debug mode with the bug since that tests round-tripping
(function() {
"use strict";

function test(op, val) {
    const coll = db.server6779;
    coll.drop();
    assert.commandWorked(coll.insert({a: true}));
    assert.commandWorked(coll.insert({a: false}));

    const obj = {};
    obj[op] = ['$a', val];
    const result = coll.aggregate([{$project: {_id: 0, bool: obj}}, {$sort: {bool: -1}}]);

    assert.eq(result.toArray(), [{bool: true}, {bool: false}]);
}
test('$and', true);
test('$or', false);
}());
