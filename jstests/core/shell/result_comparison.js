/*
 * Tests `_resultSetsEqualUnordered`, which compares two sets of results (order of documents is
 * disregarded) for equality. Field order inside an object is ignored, but array ordering and
 * everything else is required for equality.
 */

const currentDate = new Date();

// We should throw for invalid input. This function expects both arguments to be a list of objects.
assert.throwsWithCode(() => _resultSetsEqualUnordered({}, []), 9193201);
assert.throwsWithCode(() => _resultSetsEqualUnordered([], 5), 9193201);
assert.throwsWithCode(() => _resultSetsEqualUnordered([4, 1], []), 9193202);
assert.throwsWithCode(() => _resultSetsEqualUnordered([], ["abc"]), 9193203);
assert.throwsWithCode(() => _resultSetsEqualUnordered([[]], [{a: 1}]), 9193202);
assert.throwsWithCode(() => _resultSetsEqualUnordered([], undefined), 9193201);
assert.throwsWithCode(() => _resultSetsEqualUnordered([], null), 9193201);
assert.throwsWithCode(() => _resultSetsEqualUnordered([null], []), 9193202);

// Some base cases.
assert(_resultSetsEqualUnordered([], []));
assert(_resultSetsEqualUnordered([{a: 1}], [{a: 1}]));
assert(_resultSetsEqualUnordered([{a: 1}, {a: 1}], [{a: 1}, {a: 1}]));
assert(_resultSetsEqualUnordered([{a: 1}, {b: 1}], [{b: 1}, {a: 1}]));
assert(_resultSetsEqualUnordered([{a: null}], [{a: null}]));
assert(!_resultSetsEqualUnordered([], [{a: 1}]));
assert(!_resultSetsEqualUnordered([{a: 1}], []));
// Different types should fail the comparison.
assert(!_resultSetsEqualUnordered([{a: 1}], [{a: '1'}]));
assert(!_resultSetsEqualUnordered([{a: 1}], [{a: NumberLong(1)}]));
assert(!_resultSetsEqualUnordered([{a: 1}], [{a: NumberDecimal(1)}]));
assert(!_resultSetsEqualUnordered([{a: NumberInt(1)}], [{a: NumberDecimal(1)}]));
assert(!_resultSetsEqualUnordered([{a: NumberInt(1)}], [{a: NumberLong(1)}]));
assert(!_resultSetsEqualUnordered([{a: null}], [{}]));
assert(!_resultSetsEqualUnordered([{a: null}], [{b: null}]));
assert(!_resultSetsEqualUnordered([{a: null}], [{a: undefined}]));
assert(!_resultSetsEqualUnordered([{}], [{a: undefined}]));

/*
 * Given two sets of results - `equalResults` and `differentResults`, we test that all pairs of
 * results in `equalResults` are equal to each other. We also test that pairs of one result from
 * `equalResults` and one result from `differentResults` are unequal.
 */
function assertExpectedOutputs(equalResults, differentResults) {
    for (const result1 of equalResults) {
        for (const result2 of equalResults) {
            assert(_resultSetsEqualUnordered(result1, result2), {result1, result2});
            assert(_resultSetsEqualUnordered(result2, result1), {result1, result2});
        }
    }
    for (const result1 of equalResults) {
        for (const result2 of differentResults) {
            assert(!_resultSetsEqualUnordered(result1, result2), {result1, result2});
            assert(!_resultSetsEqualUnordered(result2, result1), {result1, result2});
        }
    }
}

function testIndividualDocumentEquality() {
    const doc = {
        a: 1,
        b: [
            {x: "a string", y: currentDate, z: NumberDecimal(1)},
            {'a1.b1': 5, 'a1.c1': 2, 'a2': [3, 2, 1]}
        ]
    };
    const docOutOfOrder = {
        b: [
            {z: NumberDecimal(1), x: "a string", y: currentDate},
            {'a1.b1': 5, 'a2': [3, 2, 1], 'a1.c1': 2}
        ],
        a: 1
    };

    // We change the order of arrays here - our comparator should return false, because arrays need
    // to be ordered.
    const docAltered1 = {
        a: 1,
        b: [
            {z: NumberDecimal(1), x: "a string", y: currentDate},
            {'a1.b1': 5, 'a1.c1': 2, 'a2': [1, 2, 3]}
        ]
    };
    const docAltered2 = {
        a: 1,
        b: [
            {'a1.b1': 5, 'a1.c1': 2, 'a2': [3, 2, 1]},
            {z: NumberDecimal(1), x: "a string", y: currentDate}
        ]
    };
    // Change a few values, which should also make our comparator return false.
    const docAltered3 = {
        a: 1,
        b: [
            {x: "a string", y: currentDate, z: NumberDecimal(2)},
            {'a1.b1': 5, 'a1.c1': 2, 'a2': [3, 2, 1]}
        ]
    };
    const docAltered4 = {
        a: 1,
        b: [
            {x: "a string", y: currentDate, z: NumberDecimal(1)},
            {'a1.b1': 5, 'a1.c1': 2, 'a2': [3, 3, 1]}
        ]
    };
    const docAltered5 = {
        a: 1,
        b: [
            {x: "a different string", y: currentDate, z: NumberDecimal(1)},
            {'a1.b1': 5, 'a1.c1': 2, 'a2': [3, 2, 1]}
        ]
    };

    // Each result contains one document for this case.
    const equalDocs = [[doc], [docOutOfOrder]];
    const unequalDocs = [[docAltered1], [docAltered2], [docAltered3], [docAltered4], [docAltered5]];
    assertExpectedOutputs(equalDocs, unequalDocs);
}

function testResultOrderIndifference() {
    const result = [{a: 1}, {a: 1, b: 1}, {a: 1, b: 1}, {b: 1}, {c: 1}];
    // Different order of documents.
    const resultOutOfOrder = [{b: 1, a: 1}, {c: 1}, {a: 1}, {b: 1}, {a: 1, b: 1}];
    // Change the values, or have completely different documents in the result.
    const resultAltered1 = [{a: 1, b: 1}, {d: 1}, {a: 1}, {b: 1}, {a: 1, b: 1}];
    const resultAltered2 = [{a: 1, b: 2}, {c: 1}, {a: 1}, {b: 1}, {a: 1, b: 1}];
    const resultAltered3 = [{a: 1, b: 2}, {c: 1}, {a: 1}, {b: 2}, {a: 1, b: 1}];
    const resultAltered4 = [{a: 1, b: 1}, {c: 1}, {a: 1}, {b: 1}, {'a.a': 1, b: 1}];
    const resultAltered5 = [{a: 1, b: 1}, {c: 1}, {a: 1}, {b: 1}, {a: '1', b: 1}];
    const resultAltered6 = [{a: 1, b: 1}, {c: 1}, {a: 1, b: 1}, {b: 1}, {a: 1, b: 1}];

    assertExpectedOutputs([result, resultOutOfOrder], [
        resultAltered1,
        resultAltered2,
        resultAltered3,
        resultAltered4,
        resultAltered5,
        resultAltered6
    ]);
}

testIndividualDocumentEquality();
testResultOrderIndifference();