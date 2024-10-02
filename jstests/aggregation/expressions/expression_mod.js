// Confirm correctness of $mod evaluation in aggregation.

import "jstests/libs/sbe_assert_error_override.js";

import {assertErrorCode, testExpression} from "jstests/aggregation/extras/utils.js";

var testDB = db.getSiblingDB("expression_mod");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName();
var coll = testDB.getCollection(collName);

//
// Confirm different input numeric types are evaluated correctly.
//

// Aggregate checking various combinations of number types.
// The $match portion ensures they are of the correct type as the shell turns the ints back to
// doubles at the end so we can not check types with assert.
coll.save({});
var result =
    coll.aggregate({
            $project: {
                _id: 0,
                dub_dub: {$mod: [138.5, 3.0]},
                dub_int: {$mod: [138.5, NumberInt(3)]},
                dub_long: {$mod: [138.5, NumberLong(3)]},
                int_dub: {$mod: [NumberInt(8), 3.25]},
                int_dubint: {$mod: [NumberInt(8), 3.0]},
                int_int: {$mod: [NumberInt(8), NumberInt(3)]},
                int_long: {$mod: [NumberInt(8), NumberLong(3)]},
                long_dub: {$mod: [NumberLong(8), 3.25]},
                long_dubint: {$mod: [NumberLong(8), 3.0]},
                long_dublong: {$mod: [NumberLong(500000000000), 450000000000.0]},
                long_int: {$mod: [NumberLong(8), NumberInt(3)]},
                long_long: {$mod: [NumberLong(8), NumberLong(3)]},
                verylong_verylong: {$mod: [NumberLong(800000000000), NumberLong(300000000000)]}
            }
        },
                   {
                       $project: {
                           _id: 0,
                           dub_dub: {type: {$type: "$dub_dub"}, value: "$dub_dub"},
                           dub_int: {type: {$type: "$dub_int"}, value: "$dub_int"},
                           dub_long: {type: {$type: "$dub_long"}, value: "$dub_long"},
                           int_dub: {type: {$type: "$int_dub"}, value: "$int_dub"},
                           int_dubint: {type: {$type: "$int_dubint"}, value: "$int_dubint"},
                           int_int: {type: {$type: "$int_int"}, value: "$int_int"},
                           int_long: {type: {$type: "$int_long"}, value: "$int_long"},
                           long_dub: {type: {$type: "$long_dub"}, value: "$long_dub"},
                           long_dubint: {type: {$type: "$long_dubint"}, value: "$long_dubint"},
                           long_dublong: {type: {$type: "$long_dublong"}, value: "$long_dublong"},
                           long_int: {type: {$type: "$long_int"}, value: "$long_int"},
                           long_long: {type: {$type: "$long_long"}, value: "$long_long"},
                           verylong_verylong:
                               {type: {$type: "$verylong_verylong"}, value: "$verylong_verylong"},
                       },
                   })
        .toArray();

// Correct answers (it is mainly the types that are important here).
var expectedResult = [{
    dub_dub: {type: "double", value: 0.5},
    dub_int: {type: "double", value: 0.5},
    dub_long: {type: "double", value: 0.5},
    int_dub: {type: "double", value: 1.5},
    int_dubint: {type: "double", value: 2.0},
    int_int: {type: "int", value: 2},
    int_long: {type: "long", value: NumberLong(2)},
    long_dub: {type: "double", value: 1.5},
    long_dubint: {type: "double", value: 2.0},
    long_dublong: {type: "double", value: 50000000000},
    long_int: {type: "long", value: NumberLong(2)},
    long_long: {type: "long", value: NumberLong(2)},
    verylong_verylong: {type: "long", value: NumberLong(200000000000)}
}];

assert.eq(result.length, 1, `Expected one result`);

for (const key of Object.keys(expectedResult[0])) {
    const real = result[0][key];
    const expected = expectedResult[0][key];
    assert.eq(real, expected, `Result for ${key} has incorrect type or value, or is missing`);
}

//
// Confirm error cases.
//

// Confirm mod by 0 fails in an expected manner.
assertErrorCode(coll, {$project: {a: {$mod: [10, 0 /*double*/]}}}, 16610);
assertErrorCode(coll, {$project: {a: {$mod: [NumberInt(10), NumberInt(0)]}}}, 16610);
assertErrorCode(coll, {$project: {a: {$mod: [NumberLong(10), NumberLong(0)]}}}, 16610);

// Confirm expected behavior for NaN and Infinity values.
testExpression(coll, {$mod: [10, NaN]}, NaN);
testExpression(coll, {$mod: [10, Infinity]}, 10);
testExpression(coll, {$mod: [10, -Infinity]}, 10);
testExpression(coll, {$mod: [Infinity, 10]}, NaN);
testExpression(coll, {$mod: [-Infinity, 10]}, NaN);
testExpression(coll, {$mod: [NaN, 10]}, NaN);
