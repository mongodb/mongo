/**
 * Test the $hash expression for generating hashes of string or binary inputs.
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */
import {beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];

const successTests = [
    // Normal strings
    {
        expressionInput: {input: {$concat: ["Hello", " ", "World"]}, algorithm: "xxh64"},
        expectedHash: BinData(0, "YzTSBxkkW8I="),
    },
    {
        expressionInput: {input: "[1, 2, 3]", algorithm: {$concat: ["xxh", "64"]}},
        expectedHash: BinData(0, "wkwP8yqWE2o="),
    },
    {expressionInput: {input: "🧐🤓😎", algorithm: "xxh64"}, expectedHash: BinData(0, "tWWU4BmD+Z4=")},

    {
        expressionInput: {input: {$concat: ["Hello", " ", "World"]}, algorithm: "sha256"},
        expectedHash: BinData(0, "pZGm1Av0IEBKARczz7exkNYsZb8LzaMrV7J32a2fFG4="),
    },
    {
        expressionInput: {input: "[1, 2, 3]", algorithm: {$concat: ["sha", "256"]}},
        expectedHash: BinData(0, "o2sfLD+EUi3RAFFFZGYX1wVMCFHpfHKgOcC9+sn6B/M="),
    },
    {
        expressionInput: {input: "🧐🤓😎", algorithm: "sha256"},
        expectedHash: BinData(0, "YusBsGklWsjEz6zx2aHNpEjO1tCwVeVDnEP0/O7HFuc="),
    },

    {
        expressionInput: {input: {$concat: ["Hello", " ", "World"]}, algorithm: "md5"},
        expectedHash: BinData(0, "sQqNsWTgdUEFt6mb5y4/5Q=="),
    },
    {
        expressionInput: {input: "[1, 2, 3]", algorithm: {$concat: ["md", "5"]}},
        expectedHash: BinData(0, "SaWpYMVxTC4p3Rp+e5UHQQ=="),
    },
    {
        expressionInput: {input: "🧐🤓😎", algorithm: "md5"},
        expectedHash: BinData(0, "sAZFMN8tC1iTuOxlnoqCJg=="),
    },

    // Empty
    {expressionInput: {input: "", algorithm: "xxh64"}, expectedHash: BinData(0, "70bbN1HY6Zk=")},
    {expressionInput: {input: BinData(0, ""), algorithm: "xxh64"}, expectedHash: BinData(0, "70bbN1HY6Zk=")},

    {
        expressionInput: {input: "", algorithm: "sha256"},
        expectedHash: BinData(0, "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="),
    },
    {
        expressionInput: {input: BinData(0, ""), algorithm: "sha256"},
        expectedHash: BinData(0, "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="),
    },

    {
        expressionInput: {input: "", algorithm: "md5"},
        expectedHash: BinData(0, "1B2M2Y8AsgTpgAmY7PhCfg=="),
    },
    {
        expressionInput: {input: BinData(0, ""), algorithm: "md5"},
        expectedHash: BinData(0, "1B2M2Y8AsgTpgAmY7PhCfg=="),
    },

    // Binary input
    {expressionInput: {input: BinData(0, "aGV5"), algorithm: "xxh64"}, expectedHash: BinData(0, "Wv5M8jFeEv4=")},
    {expressionInput: {input: BinData(4, "aGV5"), algorithm: "xxh64"}, expectedHash: BinData(0, "Wv5M8jFeEv4=")},

    {
        expressionInput: {input: BinData(0, "aGV5"), algorithm: "sha256"},
        expectedHash: BinData(0, "+mkLggYe39KFJimuuoqJd7V+QPy3fRp6KLJsumJZEgQ="),
    },
    {
        expressionInput: {input: BinData(4, "aGV5"), algorithm: "sha256"},
        expectedHash: BinData(0, "+mkLggYe39KFJimuuoqJd7V+QPy3fRp6KLJsumJZEgQ="),
    },

    {
        expressionInput: {input: BinData(0, "aGV5"), algorithm: "md5"},
        expectedHash: BinData(0, "YFfxPEluz3/Xd86555rihQ=="),
    },
    {
        expressionInput: {input: BinData(4, "aGV5"), algorithm: "md5"},
        expectedHash: BinData(0, "YFfxPEluz3/Xd86555rihQ=="),
    },

    // Nested
    {
        expressionInput: {input: {$hash: {input: "hey", algorithm: "xxh64"}}, algorithm: "xxh64"},
        expectedHash: BinData(0, "B+qrIpasjj4="),
    },

    {
        expressionInput: {input: {$hash: {input: "hey", algorithm: "sha256"}}, algorithm: "sha256"},
        expectedHash: BinData(0, "/dRyPNI232cCz1SqJNRJeL6p29B1c+xR25ujiTBvIrY="),
    },

    {
        expressionInput: {input: {$hash: {input: "hey", algorithm: "md5"}}, algorithm: "md5"},
        expectedHash: BinData(0, "BQ7ZtygmyG3sgzltGZh8Gg=="),
    },

    // Nullish input
    {expressionInput: {input: null, algorithm: "xxh64"}, expectedHash: null},
    {expressionInput: {input: undefined, algorithm: "sha256"}, expectedHash: null},
    {expressionInput: {input: "$missing", algorithm: "md5"}, expectedHash: null},
];

const failureTests = [
    {expressionInput: {}, expectedCode: ErrorCodes.FailedToParse},
    {expressionInput: {input: "string"}, expectedCode: ErrorCodes.FailedToParse},
    {expressionInput: {algorithm: "xxh64"}, expectedCode: ErrorCodes.FailedToParse},
    {expressionInput: {input: "string", algorithm: "xxh64", extra: 1}, expectedCode: ErrorCodes.FailedToParse},
    {expressionInput: {input: [1, 2, 3], algorithm: "xxh64"}, expectedCode: 10754000},
    {expressionInput: {input: "string", algorithm: [5]}, expectedCode: 10754001},
    {expressionInput: {input: "string", algorithm: null}, expectedCode: 10754001},
    {expressionInput: {input: "string", algorithm: "sha1"}, expectedCode: 10754002},
    {expressionInput: {input: "string", algorithm: "XXH64"}, expectedCode: 10754002},
];

describe("$hash", () => {
    beforeEach(() => {
        assertDropCollection(db, collName);
        assert.commandWorked(coll.insert({_id: 0}));
    });

    describe("succeeds", () => {
        for (const {expressionInput, expectedHash} of successTests) {
            it(`for ${tojson(expressionInput)}`, () => {
                const actualHash = coll.aggregate([{$project: {hash: {$hash: expressionInput}}}]).toArray()[0].hash;
                assert(bsonBinaryEqual(actualHash, expectedHash), `expected=${expectedHash}, actual=${actualHash}`);
            });
        }
    });

    describe("fails", () => {
        for (const {expressionInput, expectedCode} of failureTests) {
            it(`for ${tojson(expressionInput)}`, () => {
                assertErrorCode(coll, [{$project: {hash: {$hash: expressionInput}}}], expectedCode);
            });
        }
    });
});
