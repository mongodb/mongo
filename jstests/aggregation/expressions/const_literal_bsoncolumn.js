/**
 * Tests that $const and $literal reject BinData subtype 7 (Column), which is reserved for the
 * internal BSONColumn format, including when it is nested inside a document or array.
 */

const coll = db[jsTestName()];
assert(coll.drop());

const bsonColumn = BinData(7, "CAABAA==");
const genericBinData = BinData(0, "abcdefgh");

assert.commandWorked(coll.insert({_id: 0}));

function assertProjectionFails(expr) {
    assert.throwsWithCode(
        () => coll.aggregate([{$project: {output: expr}}]).toArray(),
        ErrorCodes.FailedToParse,
        [],
        expr,
    );
}

function assertProjectionWorks(constExpr) {
    assert.doesNotThrow(() => coll.aggregate([{$project: {output: constExpr}}]).toArray());
}

// Test cases for when binData is accepted or rejected when it's located in different parts of BSON
const testCases = [
    // As the constant value
    (binData) => binData,
    // As a field in an object
    (binData) => ({a: binData}),
    (binData) => ({a: 1, b: binData}),
    (binData) => ({a: binData, b: binData}),
    // In an array
    (binData) => [1, binData, 2],
    (binData) => [1, binData, 2, binData],
    // Recursive cases
    (binData) => ({a: {b: binData}}),
    (binData) => ({a: {b: {c: binData}}}),
    (binData) => ({a: [{b: binData}]}),
    (binData) => ({a: [binData]}),
    (binData) => ({a: [[binData]]}),
    (binData) => ({a: [[[binData]]]}),
    (binData) => [{a: binData}],
    (binData) => [{a: {b: binData}}],
    // In the scope of a CodeWScope value.
    (binData) => Code("return 1;", {a: binData}),
    (binData) => Code("return 1;", {a: 1, b: binData}),
    (binData) => Code("return 1;", {a: {b: binData}}),
    (binData) => Code("return 1;", {a: [binData]}),
    (binData) => Code("return 1;", {a: [{b: binData}]}),
    // CodeWScope nested inside a document or array.
    (binData) => ({a: Code("return 1;", {b: binData})}),
    (binData) => [Code("return 1;", {a: binData})],
];

for (const testCase of testCases) {
    // BSONColumn should be rejected.
    assertProjectionFails({$const: testCase(bsonColumn)});
    assertProjectionFails({$literal: testCase(bsonColumn)});
    // Generic binData should succeed.
    assertProjectionWorks({$const: testCase(genericBinData)});
    assertProjectionWorks({$literal: testCase(genericBinData)});
}
