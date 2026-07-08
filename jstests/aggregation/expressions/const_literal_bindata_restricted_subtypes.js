/**
 * Tests that $const and $literal reject restricted BinData subtypes, including when the value is
 * nested inside a document or array.
 *
 * @tags: [requires_fcv_70]
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

// Subtype 2 (ByteArrayDeprecated): first 4 bytes as LE int32 must equal (total - 4).
// 16-byte buffer: bytes[0..3] = 12 (LE int32) - correct inner prefix.
const subtype2Valid = BinData(2, "DAAAAAAAAAAAAAAAAAAAAA==");

// 16-byte buffer: all zeros - inner prefix = 0 != 12.
const subtype2BadPrefix = BinData(2, "AAAAAAAAAAAAAAAAAAAAAA==");

assertProjectionWorks({$const: subtype2Valid});
assertProjectionWorks({$literal: subtype2Valid});
assertProjectionFails({$const: subtype2BadPrefix});
assertProjectionFails({$literal: subtype2BadPrefix});

// Subtypes 3 (bdtUUID deprecated) and 5 (MD5Type): must be exactly 16 bytes.
const k16BytesBase64 = "AAAAAAAAAAAAAAAAAAAAAA==";
const k4BytesBase64 = "AAAAAA==";

assertProjectionWorks({$const: BinData(3, k16BytesBase64)});
assertProjectionWorks({$literal: BinData(3, k16BytesBase64)});
assertProjectionFails({$const: BinData(3, k4BytesBase64)});
assertProjectionFails({$literal: BinData(3, k4BytesBase64)});

assertProjectionWorks({$const: BinData(5, k16BytesBase64)});
assertProjectionWorks({$literal: BinData(5, k16BytesBase64)});
assertProjectionFails({$const: BinData(5, k4BytesBase64)});
assertProjectionFails({$literal: BinData(5, k4BytesBase64)});

// Subtype 6 (Encrypt): allowed (FLE range queries embed subtype-6 payloads as expression operands).
assertProjectionWorks({$const: BinData(6, "CAABAA==")});
assertProjectionWorks({$literal: BinData(6, "CAABAA==")});
