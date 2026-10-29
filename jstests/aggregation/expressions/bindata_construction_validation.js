/**
 * Asserts that aggregation operators cannot produce malformed BinData values.
 *
 * @tags: [
 *   # Most cases go through $function, $accumulator or mapReduce.
 *   requires_scripting,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertOne({_id: 0}));

const vectorErrorCode = 10506600;
const fixedSizeErrorCode = 13016802;
const byteArrayErrorCode = 12978505;
const columnErrorCode = 12978506;
const unassignedErrorCode = 12978507;
const column = BinData(7, "CQDoAwAAAAAAAAA=");
const encrypt = BinData(6, "CAABAA==");
const malformedVector = BinData(9, "/wA=");
const wellFormedVector = BinData(9, "AwAB");
const shortUuid = BinData(4, "AAAA"); // 3 bytes
const validUuid = BinData(4, "AAAAAAAAAAAAAAAAAAAAAA=="); // 16 bytes
const validMd5 = BinData(5, "AAAAAAAAAAAAAAAAAAAAAA=="); // 16 bytes
const validByteArray = BinData(2, "DAAAAAAAAAAAAAAAAAAAAA=="); // 16 bytes

function projectConst(value) {
    return coll.aggregate([{$project: {_id: 0, out: {$const: value}}}]).toArray();
}

function projectFunction(expr) {
    return coll
        .aggregate([
            {
                $project: {
                    _id: 0,
                    out: {
                        $function: {
                            body: `function() { return ${expr}; }`,
                            args: [],
                            lang: "js",
                        },
                    },
                },
            },
        ])
        .toArray();
}

function mapReduceEmit(value) {
    return coll.mapReduce(
        function () {
            emit(this._id, value);
        },
        function (key, values) {
            return values[0];
        },
        {out: {inline: 1}, scope: {value}},
    );
}

function accumulate(expr) {
    return coll
        .aggregate([
            {
                $group: {
                    _id: null,
                    out: {
                        $accumulator: {
                            init: "function() { return 0; }",
                            accumulate: "function(state, x) { return state; }",
                            accumulateArgs: [1],
                            merge: "function(a, b) { return a; }",
                            finalize: `function(state) { return ${expr}; }`,
                            lang: "js",
                        },
                    },
                },
            },
        ])
        .toArray();
}

describe("BinData construction validation", function () {
    it("$const rejects a malformed vector", function () {
        assert.throwsWithCode(() => projectConst(malformedVector), vectorErrorCode);
    });

    it("$const accepts a well-formed vector", function () {
        assert.eq(projectConst(wellFormedVector)[0].out, wellFormedVector);
    });

    it("$const rejects a malformed vector nested inside an object", function () {
        assert.throwsWithCode(
            () => coll.aggregate([{$project: {out: {$const: {a: [malformedVector]}}}}]).toArray(),
            vectorErrorCode,
        );
    });

    it("$const rejects a wrong-length UUID", function () {
        assert.throwsWithCode(() => projectConst(shortUuid), fixedSizeErrorCode);
    });

    it("$const accepts a valid UUID", function () {
        assert.eq(projectConst(validUuid)[0].out, validUuid);
    });

    it("$function rejects a malformed vector", function () {
        assert.throwsWithCode(() => projectFunction("BinData(9, '/wA=')"), vectorErrorCode);
    });

    it("$function accepts a well-formed vector", function () {
        assert.eq(projectFunction("BinData(9, 'AwAB')")[0].out, wellFormedVector);
    });

    it("$function rejects a malformed vector nested inside an object", function () {
        assert.throwsWithCode(() => projectFunction("{a: [BinData(9, '/wA=')]}"), vectorErrorCode);
    });

    it("$function rejects a wrong-length MD5", function () {
        assert.throwsWithCode(() => projectFunction("BinData(5, 'AAAA')"), fixedSizeErrorCode);
    });

    it("$function accepts a valid MD5", function () {
        assert.eq(projectFunction("BinData(5, 'AAAAAAAAAAAAAAAAAAAAAA==')")[0].out, validMd5);
    });

    it("$function rejects a malformed ByteArrayDeprecated", function () {
        assert.throwsWithCode(() => projectFunction("BinData(2, 'AAAA')"), byteArrayErrorCode);
    });

    it("$function accepts a valid ByteArrayDeprecated", function () {
        assert.eq(projectFunction("BinData(2, 'DAAAAAAAAAAAAAAAAAAAAA==')")[0].out, validByteArray);
    });

    it("$const rejects a Column", function () {
        assert.throwsWithCode(() => projectConst(column), columnErrorCode);
    });

    it("$function accepts a Column", function () {
        assert.eq(projectFunction("BinData(7, 'CQDoAwAAAAAAAAA=')")[0].out, column);
    });

    it("$function rejects an unassigned subtype", function () {
        assert.throwsWithCode(() => projectFunction("BinData(16, 'abcd')"), unassignedErrorCode);
    });

    it("$const accepts an Encrypt value", function () {
        assert.eq(projectConst(encrypt)[0].out, encrypt);
    });

    it("mapReduce rejects a malformed vector emitted by the map function", function () {
        assert.throwsWithCode(() => mapReduceEmit(malformedVector), vectorErrorCode);
    });

    it("mapReduce accepts a well-formed vector emitted by the map function", function () {
        assert.eq(mapReduceEmit(wellFormedVector).results[0].value, wellFormedVector);
    });

    it("$accumulator rejects a malformed vector", function () {
        assert.throwsWithCode(() => accumulate("BinData(9, '/wA=')"), vectorErrorCode);
    });

    it("$accumulator accepts a well-formed vector", function () {
        assert.eq(accumulate("BinData(9, 'AwAB')")[0].out, wellFormedVector);
    });
});
