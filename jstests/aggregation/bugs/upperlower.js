// $toUpper, $toLower tests.

t = db.jstests_aggregation_upperlower;
t.drop();

t.save({});

function assertResult(expectedUpper, expectedLower, string) {
    result = t.aggregate({
                  $project: {upper: {$toUpper: string}, lower: {$toLower: string}}
              }).toArray()[0];
    assert.eq(expectedUpper, result.upper);
    assert.eq(expectedLower, result.lower);
}

function assertException(string) {
    assert.commandFailed(
        t.runCommand('aggregate', {pipeline: [{$project: {upper: {$toUpper: string}}}]}));
    assert.commandFailed(
        t.runCommand('aggregate', {pipeline: [{$project: {lower: {$toLower: string}}}]}));
}

// Wrong number of arguments.
assertException([]);
assertException(['a', 'b']);

// Upper and lower case conversion.
assertResult('', '', '');
assertResult('', '', ['']);
assertResult('AB', 'ab', 'aB');
assertResult('AB', 'ab', ['Ab']);
assertResult('ABZ', 'abz', 'aBz');

// With non alphabet characters.
assertResult('1', '1', '1');
assertResult('1^A-A_$%.', '1^a-a_$%.', '1^a-A_$%.');
assertResult('1290B', '1290b', '1290b');
assertResult('0XFF0B', '0xff0b', '0XfF0b');

// Type coercion.
assertResult('555.5', '555.5', 555.5);
assertResult('1970-01-01T00:00:00', '1970-01-01t00:00:00', new Date(0));
assertResult('', '', null);
assertException(/abc/);

// Nested.
spec = 'aBcDeFg';
for (i = 0; i < 10; ++i) {
    assertResult('ABCDEFG', 'abcdefg', spec);
    if (i % 2 == 0) {
        spec = [{$toUpper: spec}];
    } else {
        spec = [{$toLower: spec}];
    }
}

// Utf8.
assertResult('\u0080D\u20ac', '\u0080d\u20ac', '\u0080\u0044\u20ac');
assertResult('ó', 'ó', 'ó');  // Not handled.
assertResult('Ó', 'Ó', 'Ó');  // Not handled.

// Value from field path.
t.drop();
t.save({string: '-_aB'});
assertResult('-_AB', '-_ab', '$string');
