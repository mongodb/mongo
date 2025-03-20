/*
 * Tests `_resultSetsEqualUnordered` and `_resultSetsEqualNormalized`, which compare two sets of
 * results for equality. In `_resultSetsEqualUnordered`, field order inside an object is ignored,
 * but array ordering and everything else is required for equality. In `_resultSetsEqualNormalized`,
 * array ordering is also ignored, as well as some differences regarding numeric types and
 * floating point closeness.
 */

const currentDate = new Date();

const comparisonFunctions = [_resultSetsEqualUnordered, _resultSetsEqualNormalized];

// We should throw for invalid input. These functions expect both arguments to be a list of objects.
comparisonFunctions.forEach(comparisonFn => {
    assert.throwsWithCode(() => comparisonFn({}, []), 9193201);
    assert.throwsWithCode(() => comparisonFn([], 5), 9193201);
    assert.throwsWithCode(() => comparisonFn([4, 1], []), 9193202);
    assert.throwsWithCode(() => comparisonFn([], ["abc"]), 9193203);
    assert.throwsWithCode(() => comparisonFn([[]], [{a: 1}]), 9193202);
    assert.throwsWithCode(() => comparisonFn([], undefined), 9193201);
    assert.throwsWithCode(() => comparisonFn([], null), 9193201);
    assert.throwsWithCode(() => comparisonFn([null], []), 9193202);
});

// Some base cases.
comparisonFunctions.forEach(comparisonFn => {
    assert(comparisonFn([], []));
    assert(comparisonFn([{a: 1}], [{a: 1}]));
    assert(comparisonFn([{a: 1}, {a: 1}], [{a: 1}, {a: 1}]));
    assert(comparisonFn([{a: 1}, {b: 1}], [{b: 1}, {a: 1}]));
    assert(comparisonFn([{a: null}], [{a: null}]));
    assert(!comparisonFn([], [{a: 1}]));
    assert(!comparisonFn([{a: 1}], []));
});

// Different non-numeric types should fail both comparisons.
comparisonFunctions.forEach(comparisonFn => {
    assert(!comparisonFn([{a: 1}], [{a: '1'}]));
    assert(!comparisonFn([{a: null}], [{}]));
    assert(!comparisonFn([{a: null}], [{b: null}]));
    assert(!comparisonFn([{a: null}], [{a: undefined}]));
    assert(!comparisonFn([{}], [{a: undefined}]));
});

// Different numeric types should fail the unordered comparison.
assert(!_resultSetsEqualUnordered([{a: 1}], [{a: NumberLong(1)}]));
assert(!_resultSetsEqualUnordered([{a: 1}], [{a: NumberDecimal(1)}]));
assert(!_resultSetsEqualUnordered([{a: NumberInt(1)}], [{a: NumberDecimal(1)}]));
assert(!_resultSetsEqualUnordered([{a: NumberInt(1)}], [{a: NumberLong(1)}]));
// However, they should pass the normalized comparison.
assert(_resultSetsEqualNormalized([{a: 1}], [{a: NumberLong(1)}]));
assert(_resultSetsEqualNormalized([{a: 1}], [{a: NumberDecimal(1)}]));
assert(_resultSetsEqualNormalized([{a: NumberInt(1)}], [{a: NumberDecimal(1)}]));
assert(_resultSetsEqualNormalized([{a: NumberInt(1)}], [{a: NumberLong(1)}]));

// Unordered comparison requires all values to be exactly equal.
assert(!_resultSetsEqualUnordered([{a: 0.14285714285714285}], [{a: 0.14285714285714288}]));
// Normalized comparison rounds doubles to 15 decimal places.
assert(_resultSetsEqualNormalized([{a: 0.14285714285714285}], [{a: 0.14285714285714288}]));
// Normalized comparison is sensitive to differences before the 15th decimal place.
assert(!_resultSetsEqualNormalized([{a: 0.142857142856}], [{a: 0.142857142855}]));
// Normalized comparison doesn't currently round decimals.
assert(!_resultSetsEqualNormalized([{a: NumberDecimal('0.14285714285714285')}],
                                   [{a: NumberDecimal('0.14285714285714288')}]));

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
            // Additional normalizations shouldn't make the comparison more strict.
            assert(_resultSetsEqualNormalized(result1, result2), {result1, result2});
            assert(_resultSetsEqualNormalized(result2, result1), {result1, result2});
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
