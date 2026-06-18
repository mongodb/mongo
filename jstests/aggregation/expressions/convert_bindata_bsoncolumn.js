/**
 * Tests that $convert refuses to produce BinData subtype 7 (Column), which is reserved for the
 * internal BSONColumn format. The ban must hold both when the target subtype is a literal and when
 * it is computed from a runtime expression.
 * @tags: [
 *   # BinData $convert was added in v8.0.
 *   requires_fcv_80,
 * ]
 */

const coll = db[jsTestName()];
assert(coll.drop());

const kGenericSubtype = 0;
const kBSONColumnSubtype = 7;
const kConvertToBSONColumnNotAllowedCode = 12910300;

const toBSONColumnField = {type: "binData", subtype: kBSONColumnSubtype};
const toGenericBinDataField = {type: "binData", subtype: kGenericSubtype};

assert.commandWorked(
    coll.insert({
        _id: 0,
        input: "abcdefgh",
        objectField1: toBSONColumnField,
        objectField2: toGenericBinDataField,
        a: 2,
        b: 5,
        c: -2,
    }),
);

// Each 'to' below targets BinData subtype 7 in a different way; all must be rejected.
const rejectedToSpecs = [
    // Literal naming BinData subtype 7.
    toBSONColumnField,
    // Whole 'to' resolves from a document field at runtime, so it cannot be constant-folded.
    "$objectField1",
    // 'subtype' is a constant-folded arithmetic expression that evaluates to 7.
    {type: "binData", subtype: {$add: [6, 1]}},
    // 'subtype' is an arithmetic expression over document fields
    {type: "binData", subtype: {$add: ["$a", "$b"]}},
];

// Similar to above, but cases that should be accepted since they do not involve BSONColumn
const acceptedToSpecs = [
    toGenericBinDataField,
    "$objectField2",
    {type: "binData", subtype: {$add: [2, -2]}},
    {type: "binData", subtype: {$add: ["$a", "$c"]}},
];

for (const toField of rejectedToSpecs) {
    assert.throwsWithCode(
        () =>
            coll
                .aggregate([
                    {
                        $project: {
                            output: {$convert: {input: "$input", to: toField, format: "base64"}},
                        },
                    },
                ])
                .toArray(),
        kConvertToBSONColumnNotAllowedCode,
    );
}

for (const toField of acceptedToSpecs) {
    assert.doesNotThrow(() =>
        coll.aggregate([{$project: {output: {$convert: {input: "$input", to: toField, format: "base64"}}}}]).toArray(),
    );
}
