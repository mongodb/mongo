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

    // Empty
    {expressionInput: {input: "", algorithm: "xxh64"}, expectedHash: BinData(0, "70bbN1HY6Zk=")},
    {expressionInput: {input: BinData(0, ""), algorithm: "xxh64"}, expectedHash: BinData(0, "70bbN1HY6Zk=")},

    // Binary input
    {expressionInput: {input: BinData(0, "aGV5"), algorithm: "xxh64"}, expectedHash: BinData(0, "Wv5M8jFeEv4=")},
    {expressionInput: {input: BinData(4, "aGV5"), algorithm: "xxh64"}, expectedHash: BinData(0, "Wv5M8jFeEv4=")},

    // Nested
    {
        expressionInput: {input: {$hash: {input: "hey", algorithm: "xxh64"}}, algorithm: "xxh64"},
        expectedHash: BinData(0, "B+qrIpasjj4="),
    },

    // Nullish input
    {expressionInput: {input: null, algorithm: "xxh64"}, expectedHash: null},
    {expressionInput: {input: undefined, algorithm: "xxh64"}, expectedHash: null},
    {expressionInput: {input: "$missing", algorithm: "xxh64"}, expectedHash: null},
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

    {expressionInput: {input: "string", algorithm: "md5"}, expectedCode: ErrorCodes.NotImplemented},
    {expressionInput: {input: "string", algorithm: "sha256"}, expectedCode: ErrorCodes.NotImplemented},
];

describe("$hash", () => {
    beforeEach(() => {
        assertDropCollection(db, collName);
        assert.commandWorked(coll.insert({_id: 0}));
    });

    it("works with various inputs", () => {
        for (const {expressionInput, expectedHash} of successTests) {
            const actualHash = coll.aggregate([{$project: {hash: {$hash: expressionInput}}}]).toArray()[0].hash;
            assert(
                bsonBinaryEqual(actualHash, expectedHash),
                `input=${tojson(expressionInput)}, expected=${expectedHash}, actual=${actualHash}`,
            );
        }
    });

    it("fails for various inputs", () => {
        for (const {expressionInput, expectedCode} of failureTests) {
            assertErrorCode(coll, [{$project: {hash: {$hash: expressionInput}}}], expectedCode);
        }
    });
});
