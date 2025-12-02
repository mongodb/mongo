/**
 * Tests that the $inc update operator works correctly with special floating point values (NaN and Infinity).
 * @tags: [requires_non_retryable_writes]
 */

function runTest(initialValue, incValue, expectedValue, message) {
    const coll = db[jsTestName()];
    coll.drop();

    assert.commandWorked(coll.insertOne({value: initialValue}));
    assert.commandWorked(coll.updateOne({}, {$inc: {value: incValue}}));

    const result = coll.findOne();
    if (isNaN(expectedValue)) {
        assert(isNaN(result.value), message);
    } else {
        assert.eq(result.value, expectedValue, message);
    }
}

runTest(10, NaN, NaN, "Expected NaN when incrementing a number by NaN");
runTest(NaN, 5, NaN, "Expected NaN when incrementing NaN by a number");
runTest(NaN, NaN, NaN, "Expected NaN when incrementing NaN by NaN");

runTest(NaN, Infinity, NaN, "Expected NaN when incrementing NaN by Infinity");

runTest(Infinity, 10, Infinity, "Expected Infinity when incrementing Infinity by a number");
runTest(-Infinity, 10, -Infinity, "Expected -Infinity when incrementing -Infinity by a number");

runTest(Infinity, -Infinity, NaN, "Expected NaN when adding Infinity and -Infinity");
runTest(-Infinity, Infinity, NaN, "Expected NaN when adding -Infinity and Infinity");

runTest(10, -Infinity, -Infinity, "Expected -Infinity when incrementing a number by -Infinity");
runTest(-Infinity, 100, -Infinity, "Expected -Infinity when incrementing -Infinity by a number");
runTest(Infinity, -Infinity, NaN, "Expected NaN when adding Infinity and -Infinity");
runTest(-Infinity, Infinity, NaN, "Expected NaN when adding -Infinity and Infinity");
