// Aggregation $strcasecmp tests.

t = db.jstests_aggregation_strcasecmp;
t.drop();

t.save({});

function cmp(a, b) {
    return t.aggregate({$project: {a: {$cmp: [a, b]}}}).toArray()[0].a;
}

function strcasecmp(a, b) {
    return t.aggregate({$project: {a: {$strcasecmp: [a, b]}}}).toArray()[0].a;
}

function assertException(args) {
    assert.commandFailed(
        t.runCommand('aggregate', {pipeline: [{$project: {a: {$strcasecmp: args}}}]}));
}

function assertStrcasecmp(expected, a, b) {
    assert.eq(expected, strcasecmp(a, b));
    assert.eq(-expected, strcasecmp(b, a));
}

function assertBoth(expectedStrcasecmp, expectedCmp, a, b) {
    assertStrcasecmp(expectedStrcasecmp, a, b);
    assert.eq(expectedCmp, cmp(a, b));
    assert.eq(-expectedCmp, cmp(b, a));
}

// Wrong number of arguments.
assertException([]);
assertException(['a']);
assertException(['a', 'b', 'c']);

// Basic tests.
assertBoth(0, 0, '', '');
assertBoth(-1, -1, '', 'a');
assertBoth(0, -1, 'A', 'a');
assertBoth(1, -1, 'Ab', 'a');
assertBoth(0, -1, 'Ab', 'aB');
assertBoth(1, -1, 'Bb', 'aB');
assertBoth(-1, -1, 'Bb', 'cB');
assertBoth(1, -1, 'aB', 'aa');
assertBoth(-1, -1, 'aB', 'ac');

// With non alphabet characters.
assertBoth(0, -1, 'A$_b1C?', 'a$_b1C?');
assertBoth(1, -1, 'ABC01234', 'abc0123');

// String coercion.
assertStrcasecmp(0, '1', 1);
assertStrcasecmp(0, '1.23', 1.23);
assertStrcasecmp(0, '1970-01-01T00:00:00', new Date(0));
assertStrcasecmp(0, '1970-01-01t00:00:00', new Date(0));
assertException(['abc', /abc/]);

// Extended characters.
assertBoth(0, -1, '\u0080D\u20ac', '\u0080d\u20ac');
assertBoth(1, 1, 'ó', 'Ó');  // Not treated as equal currently.

// String from field path.
t.drop();
t.save({x: 'abc'});
assertBoth(0, 1, '$x', 'ABC');
