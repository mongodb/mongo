// Tests the behavior of $ceil, $floor, $exp, $log10, $ln and $sqrt when used in agg expressions.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");        // For assertErrorCode.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.unary_numeric;
coll.drop();

// Testing behavior for common cases.
assert.commandWorked(coll.insert([
    {_id: 0, x: null},
    {_id: 1, x: undefined},
    {_id: 2},
    {_id: 3, x: 1},
    {_id: 4, x: 10},
    {_id: 5, x: 100},
    {_id: 6, x: 2.3},
    {_id: 7, x: 2.6},
    {_id: 8, x: NumberDecimal("40")},
    {_id: 9, x: NumberLong(4)},
    {_id: 10, x: NumberLong("9223372036854775807")},  // LLONG_MAX
]));

let results = coll.aggregate([
                      {
                          $project: {
                              ceil: {$ceil: "$x"},
                              exp: {$exp: "$x"},
                              floor: {$floor: "$x"},
                              ln: {$ln: "$x"},
                              log10: {$log10: "$x"},
                              sqrt: {$sqrt: "$x"},
                              abs: {$abs: "$x"},
                          }
                      },
                      {$sort: {_id: 1}}
                  ])
                  .toArray();

let expectedResults = [
    {
        _id: 0,
        "ceil": null,
        "exp": null,
        "floor": null,
        "ln": null,
        "log10": null,
        "sqrt": null,
        "abs": null
    },
    {
        _id: 1,
        "ceil": null,
        "exp": null,
        "floor": null,
        "ln": null,
        "log10": null,
        "sqrt": null,
        "abs": null
    },
    {
        _id: 2,
        "ceil": null,
        "exp": null,
        "floor": null,
        "ln": null,
        "log10": null,
        "sqrt": null,
        "abs": null
    },
    {
        _id: 3,
        "ceil": 1,
        "exp": 2.718281828459045,
        "floor": 1,
        "ln": 0,
        "log10": 0,
        "sqrt": 1,
        "abs": 1
    },
    {
        _id: 4,
        "ceil": 10,
        "exp": 22026.465794806718,
        "floor": 10,
        "ln": 2.302585092994046,
        "log10": 1,
        "sqrt": 3.1622776601683795,
        "abs": 10
    },
    {
        _id: 5,
        "ceil": 100,
        "exp": 2.6881171418161356e+43,
        "floor": 100,
        "ln": 4.605170185988092,
        "log10": 2,
        "sqrt": 10,
        "abs": 100
    },
    {
        _id: 6,
        "ceil": 3,
        "exp": 9.974182454814718,
        "floor": 2,
        "ln": 0.8329091229351039,
        "log10": 0.36172783601759284,
        "sqrt": 1.51657508881031,
        "abs": 2.3
    },
    {
        _id: 7,
        "ceil": 3,
        "exp": 13.463738035001692,
        "floor": 2,
        "ln": 0.9555114450274363,
        "log10": 0.414973347970818,
        "sqrt": 1.61245154965971,
        "abs": 2.6
    },
    {
        // Note that we do not test exp, ln, log10, and sqrt here, because they provide inexact
        // results, and there is no simple way to do an approximate comparison of two NumberDecimal
        // objects. The unit tests for these operators should give us enough coverage already.
        _id: 8,
        "ceil": NumberDecimal("40"),
        "floor": NumberDecimal("40"),
        "abs": NumberDecimal("40")
    },
    {
        _id: 9,
        "ceil": NumberLong(4),
        "exp": 54.598150033144236,
        "floor": NumberLong(4),
        "ln": 1.3862943611198906,
        "log10": 0.6020599913279624,
        "sqrt": 2,
        "abs": NumberLong(4)
    },
    {
        _id: 10,
        "ceil": NumberLong("9223372036854775807"),
        "exp": Infinity,
        "floor": NumberLong("9223372036854775807"),
        "ln": 43.66827237527655,
        "log10": 18.964889726830815,
        "sqrt": 3037000499.97605,
        "abs": NumberLong("9223372036854775807")
    },
];

// Compare each document in the 'results' array with each document in the 'expectedResults' array,
// using an approximate equality comparison for numbers. Many of the operators tested may vary in
// their last few bits depending on platform.
for (const resultDoc of results) {
    const expectedDoc = expectedResults.find(doc => (resultDoc._id === doc._id));
    assert(expectedDoc);

    for (const [key, expectedValue] of Object.entries(expectedDoc)) {
        assert(resultDoc.hasOwnProperty(key));
        const resultValue = resultDoc[key];

        let matches = false;
        if (((typeof expectedValue) == "object") && ((typeof resultValue) == "object")) {
            // NumberDecimal case.
            matches = (bsonWoCompare({value: expectedValue}, {value: resultValue}) === 0);
        } else if (isFinite(expectedValue) && isFinite(resultValue)) {
            // Regular numbers; do an approximate comparison, expecting 48 bits of precision.
            const epsilon = Math.pow(2, -48);
            const delta = Math.abs(expectedValue - resultValue);
            matches = (delta === 0) ||
                ((delta /
                  Math.min(Math.abs(expectedValue) + Math.abs(resultValue), Number.MAX_VALUE)) <
                 epsilon);
        } else {
            matches = (expectedValue === resultValue);
        }
        assert(matches,
               `Mismatched ${key} field in document with _id ${resultDoc._id} -- Expected: ${
                   expectedValue}, Actual: ${resultValue}`);
    }
}

// Testing behavior for special cases: negative numbers, 0, NaN & Infinity on expressions that don't
// error.
assert(coll.drop());

assert.commandWorked(coll.insert([
    {_id: 0, x: 0},
    {_id: 1, x: NaN},
    {_id: 2, x: Infinity},
    {_id: 3, x: -Infinity},
    {_id: 4, x: -2.6}
]));

results =
    coll.aggregate([
            {$project: {ceil: {$ceil: "$x"}, floor: {$floor: "$x"}, exp: {$ceil: {$exp: "$x"}}}},
            {$sort: {_id: 1}}
        ])
        .toArray();

assert.eq(results, [
    {
        _id: 0,
        "ceil": 0,
        "floor": 0,
        "exp": 1,
    },
    {
        _id: 1,
        "ceil": NaN,
        "floor": NaN,
        "exp": NaN,
    },
    {
        _id: 2,
        "ceil": Infinity,
        "floor": Infinity,
        "exp": Infinity,
    },
    {
        _id: 3,
        "ceil": -Infinity,
        "floor": -Infinity,
        "exp": 0,
    },
    {
        _id: 4,
        "ceil": -2,
        "floor": -3,
        "exp": 1,
    }
]);

// Testing $sqrt, $ln & $log10 success with NaN:
assert(coll.drop());

assert.commandWorked(coll.insert([{_id: 0, x: NaN}, {_id: 1, x: NumberDecimal("NaN")}]));

results = coll.aggregate([
                  {$project: {ln: {$ln: "$x"}, log10: {$log10: "$x"}, sqrt: {$sqrt: "$x"}}},
                  {$sort: {_id: 1}}
              ])
              .toArray();

assert.eq(results, [
    {_id: 0, "ln": NaN, "log10": NaN, "sqrt": NaN},
    {_id: 1, "ln": NaN, "log10": NaN, "sqrt": NumberDecimal("NaN")}
]);

// Testing error codes.
// $ln, $log10 fail for 0 and negative numbers.
// $sqrt fails for negative numbers.

// Testing failures with 0:
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: 0}]));
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28766);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28761);

// Testing $sqrt success with 0:
results = coll.aggregate([{$project: {a: {$sqrt: "$x"}}}]).toArray();
assert.eq(results, [{_id: 0, a: 0}]);

// Testing failures with NumberLong(0):
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: NumberLong(0)}]));
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28766);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28761);

// Testing $sqrt success with NumberLong(0):
results = coll.aggregate([{$project: {a: {$sqrt: "$x"}}}]).toArray();
assert.eq(results, [{_id: 0, a: 0}]);

// Testing failures with NumberDecimal(0):
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: NumberDecimal("0")}]));
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28766);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28761);

// Testing $sqrt success with NumberDecimal(0):
results = coll.aggregate([{$project: {a: {$sqrt: "$x"}}}]).toArray();
assert.eq(results, [{_id: 0, a: NumberDecimal("0")}]);

// Testing failures with negative numbers (all types):
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: -1}]));
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28766);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28761);
assertErrorCode(coll, [{$project: {a: {$sqrt: "$x"}}}], 28714);

assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: NumberLong(-1)}]));
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28766);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28761);
assertErrorCode(coll, [{$project: {a: {$sqrt: "$x"}}}], 28714);

assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: NumberDecimal("-1")}]));
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28766);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28761);
assertErrorCode(coll, [{$project: {a: {$sqrt: "$x"}}}], 28714);

// All unary numeric agg operators fail for strings:
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: "string"}]));
assertErrorCode(coll, [{$project: {a: {$abs: "$x"}}}], 28765);
assertErrorCode(coll, [{$project: {a: {$ceil: "$x"}}}], 28765);
assertErrorCode(coll, [{$project: {a: {$floor: "$x"}}}], 28765);
assertErrorCode(coll, [{$project: {a: {$exp: "$x"}}}], 28765);
assertErrorCode(coll, [{$project: {a: {$log10: "$x"}}}], 28765);
assertErrorCode(coll, [{$project: {a: {$ln: "$x"}}}], 28765);
assertErrorCode(coll, [{$project: {a: {$sqrt: "$x"}}}], 28765);
}());
