/**
 * Tests that $convert refuses to produce certain BinData subtypes and performs structural
 * validation on others.
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
        uuidInput: "867dee52-c331-484e-92d1-c56479b8e67e",
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

// Subtype 2 (ByteArrayDeprecated): banned like subtype 7
const kByteArrayDeprecatedSubtype = 2;
const kConvertToByteArrayDeprecatedNotAllowedCode = 13016800;

assert.throwsWithCode(
    () =>
        coll
            .aggregate([
                {
                    $project: {
                        output: {
                            $convert: {
                                input: "$input",
                                to: {type: "binData", subtype: kByteArrayDeprecatedSubtype},
                                format: "base64",
                            },
                        },
                    },
                },
            ])
            .toArray(),
    kConvertToByteArrayDeprecatedNotAllowedCode,
);

// Subtype 3 (bdtUUID deprecated): requires exactly 16 bytes
const kBdtUUIDSubtype = 3;
const kConvertBinDataSizeCode = 13016802;

// base64 for 16 zero bytes.
const k16BytesBase64 = "AAAAAAAAAAAAAAAAAAAAAA==";

// "$input" is "abcdefgh" which base64-decodes to 6 bytes, not 16 - must fail.
assert.throwsWithCode(
    () =>
        coll
            .aggregate([
                {
                    $project: {
                        output: {
                            $convert: {
                                input: "$input",
                                to: {type: "binData", subtype: kBdtUUIDSubtype},
                                format: "base64",
                            },
                        },
                    },
                },
            ])
            .toArray(),
    kConvertBinDataSizeCode,
);

// Exactly 16 bytes succeeds.
assert.doesNotThrow(() =>
    coll
        .aggregate([
            {
                $project: {
                    output: {
                        $convert: {
                            input: k16BytesBase64,
                            to: {type: "binData", subtype: kBdtUUIDSubtype},
                            format: "base64",
                        },
                    },
                },
            },
        ])
        .toArray(),
);

// Subtype 4 (newUUID): uuid format required; UUID::parse enforces 16-byte size
const kNewUUIDSubtype = 4;

// Non-uuid format for subtype 4 is rejected.
assert.throws(() =>
    coll
        .aggregate([
            {
                $project: {
                    output: {
                        $convert: {
                            input: "$uuidInput",
                            to: {type: "binData", subtype: kNewUUIDSubtype},
                            format: "base64",
                        },
                    },
                },
            },
        ])
        .toArray(),
);

// uuid format is required for subtype 4, and subtype 4 is required for uuid format.
assert.throws(() =>
    coll
        .aggregate([
            {
                $project: {
                    output: {
                        $convert: {
                            input: "$uuidInput",
                            to: {type: "binData", subtype: kGenericSubtype},
                            format: "uuid",
                        },
                    },
                },
            },
        ])
        .toArray(),
);

// Malformed UUID string is rejected.
assert.throws(() =>
    coll
        .aggregate([
            {
                $project: {
                    output: {
                        $convert: {
                            input: "not-a-uuid",
                            to: {type: "binData", subtype: kNewUUIDSubtype},
                            format: "uuid",
                        },
                    },
                },
            },
        ])
        .toArray(),
);

// Valid UUID string and subtype 4 succeeds.
assert.doesNotThrow(() =>
    coll
        .aggregate([
            {
                $project: {
                    output: {
                        $convert: {
                            input: "$uuidInput",
                            to: {type: "binData", subtype: kNewUUIDSubtype},
                            format: "uuid",
                        },
                    },
                },
            },
        ])
        .toArray(),
);

// Subtype 5 (MD5Type): requires exactly 16 bytes
const kMD5TypeSubtype = 5;

// "$input" is "abcdefgh" which base64-decodes to 6 bytes, not 16 - must fail.
assert.throwsWithCode(
    () =>
        coll
            .aggregate([
                {
                    $project: {
                        output: {
                            $convert: {
                                input: "$input",
                                to: {type: "binData", subtype: kMD5TypeSubtype},
                                format: "base64",
                            },
                        },
                    },
                },
            ])
            .toArray(),
    kConvertBinDataSizeCode,
);

// Exactly 16 bytes succeeds.
assert.doesNotThrow(() =>
    coll
        .aggregate([
            {
                $project: {
                    output: {
                        $convert: {
                            input: k16BytesBase64,
                            to: {type: "binData", subtype: kMD5TypeSubtype},
                            format: "base64",
                        },
                    },
                },
            },
        ])
        .toArray(),
);

// Subtype 6 (Encrypt): banned like subtype 7
const kEncryptSubtype = 6;
const kConvertToEncryptNotAllowedCode = 13016801;

assert.throwsWithCode(
    () =>
        coll
            .aggregate([
                {
                    $project: {
                        output: {
                            $convert: {
                                input: "$input",
                                to: {type: "binData", subtype: kEncryptSubtype},
                                format: "base64",
                            },
                        },
                    },
                },
            ])
            .toArray(),
    kConvertToEncryptNotAllowedCode,
);
