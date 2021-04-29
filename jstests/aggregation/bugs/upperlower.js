// $toUpper, $toLower tests.
(function() {
'use strict';

load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.jstests_aggregation_upperlower;
coll.drop();
assert.commandWorked(coll.insert({}));
function assertResult(expectedUpper, expectedLower, string) {
    const result =
        coll.aggregate({$project: {upper: {$toUpper: string}, lower: {$toLower: string}}})
            .toArray()[0];
    assert.eq(expectedUpper, result.upper);
    assert.eq(expectedLower, result.lower);
}

function assertException(string) {
    const error1 = assert.throws(() => coll.aggregate([{$project: {upper: {$toUpper: string}}}]));
    assert.commandFailedWithCode(error1, [16020, 16007, ErrorCodes.TypeMismatch]);

    const error2 = assert.throws(() => coll.aggregate([{$project: {lower: {$toLower: string}}}]));
    assert.commandFailedWithCode(error2, [16020, 16007, ErrorCodes.TypeMismatch]);
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
assertResult('A\0B\0\0Z', 'a\0b\0\0z', 'a\0B\0\0z');
assertResult('A\0\0B\0Z', 'a\0\0b\0z', ['A\0\0b\0z']);

// With non alphabet characters.
assertResult('1', '1', '1');
assertResult('1^A-A_$%.', '1^a-a_$%.', '1^a-A_$%.');
assertResult('1290B', '1290b', '1290b');
assertResult('0XFF0B', '0xff0b', '0XfF0b');

// Type coercion.
assertResult('555.5', '555.5', 555.5);
assertResult('1970-01-01T00:00:00.000Z', '1970-01-01t00:00:00.000z', new Date(0));
assertResult('', '', null);
assertException(/abc/);
assertException(true);

// Nested.
let spec = 'aBcDeFg';
for (let i = 0; i < 10; ++i) {
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
assert(coll.drop());
assert.commandWorked(coll.insert({
    string: "-_ab",
    longString: "abcdefghijklmnopQRSTUVWXYZ123456789",
    numberLong: NumberLong("2090845886852"),
    numberInt: NumberInt(42),
    numberDecimal: NumberDecimal(42.213),
    nested: {str: "hello world"},
    unicode: "\u1ebd",
    string_with_nulls_1: "a\0B\0\0z",
    string_with_nulls_2: "A\0\0b\0z"
}));
assertResult('-_AB', '-_ab', '$string');
assertResult(
    'ABCDEFGHIJKLMNOPQRSTUVWXYZ123456789', 'abcdefghijklmnopqrstuvwxyz123456789', '$longString');
assertResult('2090845886852', '2090845886852', '$numberLong');
assertResult('42', '42', '$numberInt');
assertResult('HELLO WORLD', 'hello world', '$nested.str');
assertResult('\u1ebd', '\u1ebd', '$unicode');
assertResult('A\0B\0\0Z', 'a\0b\0\0z', '$string_with_nulls_1');
assertResult('A\0\0B\0Z', 'a\0\0b\0z', '$string_with_nulls_2');
}());
