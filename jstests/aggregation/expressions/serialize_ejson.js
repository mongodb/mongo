/**
 * Test the $serializeEJSON and $deserializeEJSON expressions.
 * @tags: [
 *  requires_fcv_83
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const collName = jsTestName();
const coll = db[collName];

function mqlNestedJSON(depth, inner = null) {
    depth = depth < 1 ? 1 : depth;
    return {$toArray: "[".repeat(depth - 1) + JSON.stringify(inner) + "]".repeat(depth - 1)};
}

function getParameter(paramName) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [paramName]: 1}))[paramName];
}

const maxBSONDepth = getParameter("maxBSONDepth");

function populateCollection(docs) {
    assertDropCollection(db, collName);
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < docs.length; i++) {
        bulk.insert({_id: i, ...docs[i]});
    }
    assert.commandWorked(bulk.execute());
}

const commonSuccessTests = [
    {bson: "string", canonical: "string", relaxed: "string"},
    {
        bson: NumberLong(1),
        canonical: {$numberLong: "1"},
        relaxed: NumberLong(1),
    },
    {
        bson: NumberInt(1),
        canonical: {$numberInt: "1"},
        relaxed: NumberInt(1),
    },
    {
        bson: {b: NumberInt(1)},
        canonical: {b: {$numberInt: "1"}},
        relaxed: {b: NumberInt(1)},
    },
    {
        bson: {b: NumberLong(123)},
        canonical: {b: {$numberLong: "123"}},
        relaxed: {b: NumberLong(123)},
    },
    {
        bson: [NumberInt(1), NumberInt(2)],
        canonical: [{$numberInt: "1"}, {$numberInt: "2"}],
        relaxed: [NumberInt(1), NumberInt(2)],
    },
    {
        bson: 1,
        canonical: {$numberDouble: "1"},
        relaxed: 1,
    },
    {
        bson: NaN,
        canonical: {$numberDouble: "NaN"},
        relaxed: {$numberDouble: "NaN"},
    },
    {
        bson: ObjectId("507f1f77bcf86cd799439011"),
        canonical: {$oid: "507f1f77bcf86cd799439011"},
        relaxed: {$oid: "507f1f77bcf86cd799439011"},
    },
    {
        bson: NumberDecimal("123"),
        canonical: {$numberDecimal: "123"},
        relaxed: {$numberDecimal: "123"},
    },
    {
        bson: Code("function(){}"),
        canonical: {$code: "function(){}"},
        relaxed: {$code: "function(){}"},
    },
    {
        bson: Code("function(){}", {a: NumberInt(1)}),
        canonical: {$code: "function(){}", $scope: {a: {$numberInt: "1"}}},
        relaxed: {$code: "function(){}", $scope: {a: {$numberInt: "1"}}},
    },
    {
        bson: ISODate("2004-03-21T18:59:54.000Z"),
        canonical: {$date: {$numberLong: "1079895594000"}},
        relaxed: {$date: "2004-03-21T18:59:54.000Z"},
    },
    {
        bson: null,
        canonical: null,
        relaxed: null,
    },
    {
        bson: undefined,
        canonical: {$undefined: true},
        relaxed: {$undefined: true},
    },
];

const serializeSuccessTests = [
    ...commonSuccessTests,
    {
        // bson is missing
        canonical: null,
        relaxed: null,
    },
];

describe("$serializeEJSON", () => {
    it("works with various inputs", () => {
        populateCollection(serializeSuccessTests);
        const results = coll
            .aggregate([
                {
                    $set: {
                        relaxedTest: {$serializeEJSON: {input: "$bson", relaxed: true}},
                        canonicalTest: {$serializeEJSON: {input: "$bson", relaxed: false}},
                    },
                },
            ])
            .toArray();
        for (const result of results) {
            assert(
                bsonBinaryEqual(result.relaxedTest, result.relaxed),
                `Failed relaxed test: ${tojson([result.relaxedTest, result.relaxed])}`,
            );
            assert(
                bsonBinaryEqual(result.canonicalTest, result.canonical),
                `Failed canonical test: ${tojson([result.canonicalTest, result.canonical])}`,
            );
        }
    });
    it("defaults relaxed to true", () => {
        assert(
            !bsonBinaryEqual(
                coll.findOne({}, {a: {$serializeEJSON: {input: NumberInt(1)}}}).a,
                coll.findOne({}, {a: {$serializeEJSON: {input: NumberInt(1), relaxed: false}}}).a,
            ),
        );
    });
    it("rejects additional parameters", () => {
        const err = assert.throwsWithCode(
            () => coll.findOne({}, {a: {$serializeEJSON: {input: {}, foo: "bar"}}}),
            ErrorCodes.FailedToParse,
        );
        assert.neq(-1, err.message.indexOf("$serializeEJSON found an unknown argument: foo"));
    });
    it("rejects non-boolean relaxed parameter", () => {
        assert.doesNotThrow(() => coll.findOne({}, {a: {$serializeEJSON: {input: {}, relaxed: true}}}));
        const err = assert.throwsWithCode(
            () => coll.findOne({}, {a: {$serializeEJSON: {input: {}, relaxed: 1}}}),
            ErrorCodes.BadValue,
        );
        assert.neq(-1, err.message.indexOf("Unexpected value for relaxed: 1"), err.message);
    });
    it("fails with conversion failure on depth limit", () => {
        // Construct an expression with has an $serializeEJSON expression.
        // The result is not returned, to avoid triggering the crashOnInvalidBSONError testing parameter.
        function serializeTest(input, relaxed) {
            return coll.findOne({}, {a: {$eq: [null, {$serializeEJSON: {input, relaxed}}]}});
        }

        assert.doesNotThrow(() => serializeTest(mqlNestedJSON(maxBSONDepth, 1), true));
        assert.throwsWithCode(() => serializeTest(mqlNestedJSON(maxBSONDepth, 1), false), ErrorCodes.ConversionFailure);
    });
    it("uses onError on bad input", () => {
        const badInput = mqlNestedJSON(maxBSONDepth, 1);
        const res = coll.findOne({}, {a: {$serializeEJSON: {input: badInput, relaxed: false, onError: "depth limit"}}});
        assert.eq(res.a, "depth limit");
    });
    it("supports roundtrip using $toString", () => {
        const mqlEJSONRoundtrip = (input, relaxed) => ({
            // BSON -> EJSON -> string -> EJSON -> BSON
            $deserializeEJSON: {input: {$toObject: {$toString: {$serializeEJSON: {input, relaxed}}}}},
        });
        populateCollection(serializeSuccessTests);
        const results = coll
            .aggregate([
                {
                    $set: {
                        relaxedTest: mqlEJSONRoundtrip({bson: "$bson"}, true),
                        canonicalTest: mqlEJSONRoundtrip({bson: "$bson"}, false),
                    },
                },
            ])
            .toArray();
        for (const result of results) {
            // Relaxed mode can lose information, so we check it conditionally.
            if (bsonBinaryEqual(result.relaxed, result.canonical)) {
                assert(
                    bsonBinaryEqual(result.relaxedTest.bson, result.bson),
                    `Failed relaxed test: ${tojson([result.relaxedTest.bson, result.bson])}`,
                );
            }
            assert(
                bsonBinaryEqual(result.canonicalTest.bson, result.bson),
                `Failed canonical test: ${tojson([result.canonicalTest.bson, result.bson])}`,
            );
        }
    });
});

const deserializeSuccessTests = [
    ...commonSuccessTests,
    {
        bson: null,
        // canonical is missing
        // relaxed is missing
    },
];

const deserializeFailTests = [{$numberLong: 1}, {$numberLong: "bad"}, {$numberLong: "--1"}, {$numberLong: "Infinity"}];

describe("$deserializeEJSON", () => {
    it("works with various inputs", () => {
        populateCollection(deserializeSuccessTests);
        const results = coll
            .aggregate([
                {
                    $set: {
                        relaxedTest: {$deserializeEJSON: {input: "$relaxed"}},
                        canonicalTest: {$deserializeEJSON: {input: "$canonical"}},
                    },
                },
            ])
            .toArray();
        for (const result of results) {
            assert(
                bsonBinaryEqual(result.relaxedTest, result.bson),
                `Failed relaxed test: ${tojson([result.relaxedTest, result.bson])}`,
            );
            assert(
                bsonBinaryEqual(result.canonicalTest, result.bson),
                `Failed canonical test: ${tojson([result.canonicalTest, result.bson])}`,
            );
        }
    });
    it("rejects additional parameters", () => {
        const err = assert.throwsWithCode(
            () => coll.findOne({}, {a: {$deserializeEJSON: {input: {}, foo: "bar"}}}),
            ErrorCodes.FailedToParse,
        );
        assert.neq(-1, err.message.indexOf("$deserializeEJSON found an unknown argument: foo"));
    });
    it("supports deprecated symbol type", () => {
        // The symbol type is not supported by this environment.
        // Values of the type cannot be constructed and when returned, values are converted to strings.
        // We can assert that it is constructed correctly in pipelines.
        assertDropCollection(db, collName);
        coll.insert({input: {$symbol: "value"}});
        const res = coll.findOne(
            {},
            {value: {$deserializeEJSON: {input: "$input"}}, type: {$type: {$deserializeEJSON: {input: "$input"}}}},
        );
        assert.eq("symbol", res.type);
        assert.eq("value", res.value);
    });
    it("fails with conversion failure", () => {
        for (let test of deserializeFailTests) {
            assert.throwsWithCode(
                () => coll.findOne({}, {a: {$deserializeEJSON: {input: {$literal: test}}}}),
                ErrorCodes.ConversionFailure,
            );
        }
    });
    it("uses onError on bad input", () => {
        for (let test of deserializeFailTests) {
            const res = assert.doesNotThrow(() =>
                coll.findOne({}, {a: {$deserializeEJSON: {input: {$literal: test}, onError: "failed"}}}),
            );
            assert.eq(res.a, "failed");
        }
    });
});
