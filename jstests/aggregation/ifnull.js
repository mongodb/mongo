(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const t = db.jstests_aggregation_ifnull;
t.drop();
assert.commandWorked(t.insertOne({
    zero: 0,
    one: 1,
    two: 2,
    three: 3,
    my_false: false,
    my_str: '',
    my_null: null,
    my_undefined: undefined,
    my_obj: {},
    my_list: []
}));

function assertError(expectedErrorCode, ifNullSpec) {
    assertErrorCode(t, {$project: {a: {$ifNull: ifNullSpec}}}, expectedErrorCode);
}

function assertResult(expectedResult, ifNullSpec) {
    const res = t.aggregate({$project: {_id: 0, a: {$ifNull: ifNullSpec}}}).toArray()[0];
    assert.docEq({a: expectedResult}, res);
}

// Wrong number of args.
assertError(1257300, []);
assertError(1257300, ['$one']);
assertError(1257300, ['$my_null']);

// First arg non null.
assertResult(1, ['$one', '$two']);
assertResult(2, ['$two', '$one']);
assertResult(false, ['$my_false', '$one']);
assertResult('', ['$my_str', '$one']);
assertResult([], ['$my_list', '$one']);
assertResult({}, ['$my_obj', '$one']);
assertResult(1, ['$one', '$my_null']);
assertResult(2, ['$two', '$my_undefined']);
assertResult(1, ['$one', '$two', '$three']);
assertResult(1, ['$one', '$two', '$my_null']);
assertResult(1, ['$one', '$my_null', '$two']);

// First arg null.
assertResult(2, ['$my_null', '$two']);
assertResult(1, ['$my_null', '$one']);
assertResult(null, ['$my_null', '$my_null']);
assertResult(false, ['$my_null', '$my_false', '$one']);
assertResult(false, ['$my_null', '$my_null', '$my_false']);
assertResult(null, ['$my_null', '$my_null', '$my_null', '$my_null']);

// First arg undefined.
assertResult(2, ['$my_undefined', '$two']);
assertResult(1, ['$my_undefined', '$one']);
assertResult(null, ['$my_undefined', '$my_null']);
assertResult(false, ['$my_undefined', '$my_false', '$one']);
assertResult('', ['$my_undefined', '$my_null', '$missingField', '$my_str', '$two']);

// Computed expression.
assertResult(2, [{$add: ['$one', '$one']}, '$three']);
assertResult(6, ['$missingField', {$multiply: ['$two', '$three']}]);
assertResult(2, [{$add: ['$one', '$one']}, '$three', '$zero']);

// Divide/mod by 0.
assertError([16608, 4848401], [{$divide: ['$one', '$zero']}, '$zero']);
assertError([16610, 4848403], [{$mod: ['$one', '$zero']}, '$zero']);

// Return undefined.
assertResult(undefined, ['$my_null', '$my_undefined']);
assertResult(undefined, ['$my_undefined', '$my_undefined']);

// Nested.
assert(t.drop());
assert.commandWorked(t.insertOne({d: 'foo'}));
assertResult('foo', ['$a', {$ifNull: ['$b', {$ifNull: ['$c', '$d']}]}]);
assert.commandWorked(t.updateMany({}, {$set: {b: 'bar'}}));
assertResult('bar', ['$a', {$ifNull: ['$b', {$ifNull: ['$c', '$d']}]}]);
assertResult('bar', ['$a', {$ifNull: ['$b', {$ifNull: ['$c', '$d']}]}, '$e']);
}());