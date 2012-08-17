// server 6779: serializing ExpressionCoerceToBool
// This test only fails in debug mode with the bug since that tests round-tripping
function test(op, val) {
    t = db.server6779;
    t.drop();

    t.insert({a:true});
    t.insert({a:false});

    obj = {};
    obj[op] = ['$a', val];
    result = t.aggregate({$project: {_id: 0, bool: obj}});

    assert.commandWorked(result);
    assert.eq(result.result, [{bool:true}, {bool:false}]);
}
test('$and', true);
test('$or', false);
