// $ifNull returns the result of the first expression if not null or undefined, otherwise of the
// second expression.
load('jstests/aggregation/extras/utils.js');

t = db.jstests_aggregation_ifnull;
t.drop();

t.save({});

function assertError(expectedErrorCode, ifNullSpec) {
    assertErrorCode(t, {$project: {a: {$ifNull: ifNullSpec}}}, expectedErrorCode);
}

function assertResult(expectedResult, arg0, arg1) {
    var res = t.aggregate({$project: {a: {$ifNull: [arg0, arg1]}}}).toArray()[0];
    assert.eq(expectedResult, res.a);
}

// Wrong number of args.
assertError(16020, []);
assertError(16020, [1]);
assertError(16020, [null]);
assertError(16020, [1, 1, 1]);
assertError(16020, [1, 1, null]);
assertError(16020, [1, 1, undefined]);

// First arg non null.
assertResult(1, 1, 2);
assertResult(2, 2, 1);
assertResult(false, false, 1);
assertResult('', '', 1);
assertResult([], [], 1);
assertResult({}, {}, 1);
assertResult(1, 1, null);
assertResult(2, 2, undefined);

// First arg null.
assertResult(2, null, 2);
assertResult(1, null, 1);
assertResult(null, null, null);
assertResult(undefined, null, undefined);

// First arg undefined.
assertResult(2, undefined, 2);
assertResult(1, undefined, 1);
assertResult(null, undefined, null);
assertResult(undefined, undefined, undefined);

// Computed expression.
assertResult(3, {$add: [1, 2]}, 5);
assertResult(20, '$missingField', {$multiply: [4, 5]});

// Divide/mod by 0.
assertError(16608, [{$divide: [1, 0]}, 0]);
assertError(16610, [{$mod: [1, 0]}, 0]);

// Nested.
t.drop();
t.save({d: 'foo'});
assertResult('foo', '$a', {$ifNull: ['$b', {$ifNull: ['$c', '$d']}]});
t.update({}, {$set: {b: 'bar'}});
assertResult('bar', '$a', {$ifNull: ['$b', {$ifNull: ['$c', '$d']}]});
