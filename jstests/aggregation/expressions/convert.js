/**
 * Tests behavior of $convert aggregation operator.
 */
(function() {
"use strict";

const coll = db.expression_convert;
function populateCollection(documentList) {
    coll.drop();
    var bulk = coll.initializeOrderedBulkOp();
    documentList.forEach(doc => bulk.insert(doc));
    assert.writeOK(bulk.execute());
}

//
// One test document for each possible conversion. Edge cases for these conversions are tested
// in expression_convert_test.cpp.
//
var conversionTestDocs = [
    {_id: 0, input: 1.9, target: "double", expected: 1.9},
    {_id: 1, input: 1.9, target: "string", expected: "1.9"},
    {_id: 2, input: 1.9, target: "bool", expected: true},
    {_id: 3, input: 1.9, target: "date", expected: ISODate("1970-01-01T00:00:00.001Z")},
    {_id: 4, input: 1.9, target: "int", expected: NumberInt(1)},
    {_id: 5, input: 1.9, target: "long", expected: NumberLong(1)},
    {_id: 6, input: 1.9, target: "decimal", expected: NumberDecimal(1.9)},

    {_id: 7, input: "1.9", target: "double", expected: 1.9},
    {_id: 8, input: "str", target: "string", expected: "str"},
    {
        _id: 9,
        input: "0123456789abcdef01234567",
        target: "objectId",
        expected: ObjectId("0123456789abcdef01234567")
    },
    {_id: 10, input: "", target: "bool", expected: true},
    {
        _id: 11,
        input: "1970-01-01T00:00:00.001Z",
        target: "date",
        expected: ISODate("1970-01-01T00:00:00.001Z")
    },
    {_id: 12, input: "1", target: "int", expected: NumberInt(1)},
    {_id: 13, input: "1", target: "long", expected: NumberLong(1)},
    {_id: 14, input: "1.9", target: "decimal", expected: NumberDecimal("1.9")},

    {
        _id: 15,
        input: ObjectId("0123456789abcdef01234567"),
        target: "string",
        expected: "0123456789abcdef01234567"
    },
    {_id: 16, input: ObjectId("0123456789abcdef01234567"), target: "bool", expected: true},
    {
        _id: 17,
        input: ObjectId("0123456789abcdef01234567"),
        target: "objectId",
        expected: ObjectId("0123456789abcdef01234567")
    },
    {
        _id: 18,
        input: ObjectId("0123456789abcdef01234567"),
        target: "date",
        expected: ISODate("1970-08-09T22:25:43Z")
    },

    {_id: 19, input: false, target: "double", expected: 0.0},
    {_id: 20, input: false, target: "string", expected: "false"},
    {_id: 21, input: false, target: "bool", expected: false},
    {_id: 22, input: false, target: "int", expected: NumberInt(0)},
    {_id: 23, input: false, target: "long", expected: NumberLong(0)},
    {_id: 24, input: false, target: "decimal", expected: NumberDecimal(0)},

    {_id: 25, input: ISODate("1970-01-01T00:00:00.123Z"), target: "double", expected: 123.0},
    {
        _id: 26,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "string",
        expected: "1970-01-01T00:00:00.123Z"
    },
    {_id: 27, input: ISODate("1970-01-01T00:00:00.123Z"), target: "bool", expected: true},
    {
        _id: 28,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "date",
        expected: ISODate("1970-01-01T00:00:00.123Z")
    },
    {
        _id: 29,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "long",
        expected: NumberLong(123)
    },
    {
        _id: 30,
        input: ISODate("1970-01-01T00:00:00.123Z"),
        target: "decimal",
        expected: NumberDecimal("123")
    },

    {_id: 31, input: NumberInt(1), target: "double", expected: 1.0},
    {_id: 32, input: NumberInt(1), target: "string", expected: "1"},
    {_id: 33, input: NumberInt(1), target: "bool", expected: true},
    {_id: 34, input: NumberInt(1), target: "int", expected: NumberInt(1)},
    {_id: 35, input: NumberInt(1), target: "long", expected: NumberLong(1)},
    {_id: 36, input: NumberInt(1), target: "decimal", expected: NumberDecimal("1")},

    {_id: 37, input: NumberLong(1), target: "double", expected: 1.0},
    {_id: 38, input: NumberLong(1), target: "string", expected: "1"},
    {_id: 39, input: NumberLong(1), target: "bool", expected: true},
    {_id: 40, input: NumberLong(1), target: "date", expected: ISODate("1970-01-01T00:00:00.001Z")},
    {_id: 41, input: NumberLong(1), target: "int", expected: NumberInt(1)},
    {_id: 42, input: NumberLong(1), target: "long", expected: NumberLong(1)},
    {_id: 43, input: NumberLong(1), target: "decimal", expected: NumberDecimal("1")},

    {_id: 44, input: NumberDecimal("1.9"), target: "double", expected: 1.9},
    {_id: 45, input: NumberDecimal("1.9"), target: "string", expected: "1.9"},
    {_id: 46, input: NumberDecimal("1.9"), target: "bool", expected: true},
    {
        _id: 47,
        input: NumberDecimal("1.9"),
        target: "date",
        expected: ISODate("1970-01-01T00:00:00.001Z")
    },
    {_id: 48, input: NumberDecimal("1.9"), target: "int", expected: NumberInt(1)},
    {_id: 49, input: NumberDecimal("1.9"), target: "long", expected: NumberLong(1)},
    {_id: 50, input: NumberDecimal("1.9"), target: "decimal", expected: NumberDecimal("1.9")},

    {_id: 51, input: MinKey, target: "bool", expected: true},
    {_id: 52, input: {foo: 1, bar: 2}, target: "bool", expected: true},
    {_id: 53, input: [1, 2], target: "bool", expected: true},
    {_id: 54, input: BinData(0, "BBBBBBBBBBBBBBBBBBBBBBBBBBBB"), target: "bool", expected: true},
    {_id: 55, input: /B*/, target: "bool", expected: true},
    {_id: 56, input: new DBRef("db.test", "oid"), target: "bool", expected: true},
    {_id: 57, input: function() {}, target: "bool", expected: true},
    // Symbol and CodeWScope are not supported from JavaScript, so we can't test them here.
    {_id: 58, input: new Timestamp(1 / 1000, 1), target: "bool", expected: true},
    {_id: 59, input: MinKey, target: "bool", expected: true}
];
populateCollection(conversionTestDocs);

// Test $convert on each document.
var pipeline = [
    {
        $project: {
            output: {$convert: {to: "$target", input: "$input"}},
            target: "$target",
            expected: "$expected"
        }
    },
    {$addFields: {outputType: {$type: "$output"}}},
    {$sort: {_id: 1}}
];
var aggResult = coll.aggregate(pipeline).toArray();
assert.eq(aggResult.length, conversionTestDocs.length);

aggResult.forEach(doc => {
    assert.eq(doc.output, doc.expected, "Unexpected conversion: _id = " + doc._id);
    assert.eq(doc.outputType, doc.target, "Conversion to incorrect type: _id = " + doc._id);
});

// Test each conversion using the shorthand $toBool, $toString, etc. syntax.
pipeline = [
    {
        $project: {
            output: {
                $switch: {
                    branches: [
                        {case: {$eq: ["$target", "double"]}, then: {$toDouble: "$input"}},
                        {case: {$eq: ["$target", "string"]}, then: {$toString: "$input"}},
                        {case: {$eq: ["$target", "objectId"]}, then: {$toObjectId: "$input"}},
                        {case: {$eq: ["$target", "bool"]}, then: {$toBool: "$input"}},
                        {case: {$eq: ["$target", "date"]}, then: {$toDate: "$input"}},
                        {case: {$eq: ["$target", "int"]}, then: {$toInt: "$input"}},
                        {case: {$eq: ["$target", "long"]}, then: {$toLong: "$input"}},
                        {case: {$eq: ["$target", "decimal"]}, then: {$toDecimal: "$input"}}
                    ]
                }
            },
            target: "$target",
            expected: "$expected"
        }
    },
    {$addFields: {outputType: {$type: "$output"}}},
    {$sort: {_id: 1}}
];
aggResult = coll.aggregate(pipeline).toArray();
assert.eq(aggResult.length, conversionTestDocs.length);

aggResult.forEach(doc => {
    assert.eq(doc.output, doc.expected, "Unexpected conversion: _id = " + doc._id);
    assert.eq(doc.outputType, doc.target, "Conversion to incorrect type: _id = " + doc._id);
});

// Test a $convert expression with "onError" to make sure that error handling still allows an
// error in the "input" expression to propagate.
assert.throws(function() {
    coll.aggregate([
        {$project: {output: {$convert: {to: "string", input: {$divide: [1, 0]}, onError: "ERROR"}}}}
    ]);
}, [], "Pipeline should have failed");

//
// Unsupported conversions.
//
var illegalConversionTestDocs = [
    {_id: 0, input: 1.9, target: "objectId"},

    {_id: 1, input: ObjectId("0123456789abcdef01234567"), target: "double"},
    {_id: 2, input: ObjectId("0123456789abcdef01234567"), target: "int"},
    {_id: 3, input: ObjectId("0123456789abcdef01234567"), target: "long"},
    {_id: 4, input: ObjectId("0123456789abcdef01234567"), target: "decimal"},

    {_id: 5, input: false, target: "objectId"},
    {_id: 6, input: false, target: "date"},

    {_id: 7, input: ISODate("1970-01-01T00:00:00.123Z"), target: "objectId"},
    {_id: 8, input: ISODate("1970-01-01T00:00:00.123Z"), target: "int"},

    {_id: 9, input: NumberInt(1), target: "objectId"},
    {_id: 10, input: NumberInt(1), target: "date"},

    {_id: 11, input: NumberLong(1), target: "objectId"},

    {_id: 12, input: NumberDecimal("1.9"), target: "objectId"},

    {_id: 13, input: 1.9, target: "minKey"},
    {_id: 14, input: 1.9, target: "missing"},
    {_id: 15, input: 1.9, target: "object"},
    {_id: 16, input: 1.9, target: "array"},
    {_id: 17, input: 1.9, target: "binData"},
    {_id: 18, input: 1.9, target: "undefined"},
    {_id: 19, input: 1.9, target: "null"},
    {_id: 20, input: 1.9, target: "regex"},
    {_id: 21, input: 1.9, target: "dbPointer"},
    {_id: 22, input: 1.9, target: "javascript"},
    {_id: 23, input: 1.9, target: "symbol"},
    {_id: 24, input: 1.9, target: "javascriptWithScope"},
    {_id: 25, input: 1.9, target: "timestamp"},
    {_id: 26, input: 1.9, target: "maxKey"},
];
populateCollection(illegalConversionTestDocs);

// Test each document to ensure that the conversion throws an error.
illegalConversionTestDocs.forEach(doc => {
    pipeline = [
        {$match: {_id: doc._id}},
        {$project: {output: {$convert: {to: "$target", input: "$input"}}}}
    ];

    assert.throws(function() {
        coll.aggregate(pipeline);
    }, [], "Conversion should have failed: _id = " + doc._id);
});

// Test that each illegal conversion uses the 'onError' value.
pipeline = [
    {$project: {output: {$convert: {to: "$target", input: "$input", onError: "ERROR"}}}},
    {$sort: {_id: 1}}
];
var aggResult = coll.aggregate(pipeline).toArray();
assert.eq(aggResult.length, illegalConversionTestDocs.length);

aggResult.forEach(doc => {
    assert.eq(doc.output, "ERROR", "Unexpected result: _id = " + doc._id);
});

// Test that, when onError is missing, the missing value propagates to the result.
pipeline = [
    {
        $project:
            {_id: false, output: {$convert: {to: "$target", input: "$input", onError: "$$REMOVE"}}}
    },
    {$sort: {_id: 1}}
];
var aggResult = coll.aggregate(pipeline).toArray();
assert.eq(aggResult.length, illegalConversionTestDocs.length);

aggResult.forEach(doc => {
    assert.eq(doc, {});
});

//
// One test document for each "nullish" value.
//
var nullTestDocs =
    [{_id: 0, input: null}, {_id: 1, input: undefined}, {_id: 2, /* input is missing */}];
populateCollection(nullTestDocs);

// Test that all nullish inputs result in the 'onNull' output.
pipeline = [
    {$project: {output: {$convert: {to: "int", input: "$input", onNull: "NULL"}}}},
    {$sort: {_id: 1}}
];
var aggResult = coll.aggregate(pipeline).toArray();
assert.eq(aggResult.length, nullTestDocs.length);

aggResult.forEach(doc => {
    assert.eq(doc.output, "NULL", "Unexpected result: _id = " + doc._id);
});

// Test that all nullish inputs result in the 'onNull' output _even_ if 'to' is nullish.
pipeline = [
    {$project: {output: {$convert: {to: null, input: "$input", onNull: "NULL"}}}},
    {$sort: {_id: 1}}
];
var aggResult = coll.aggregate(pipeline).toArray();
assert.eq(aggResult.length, nullTestDocs.length);

aggResult.forEach(doc => {
    assert.eq(doc.output, "NULL", "Unexpected result: _id = " + doc._id);
});
}());
