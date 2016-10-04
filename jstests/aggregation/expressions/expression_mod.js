// Confirm correctness of $mod evaluation in aggregation.

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.

(function() {
    "use strict";

    var testDB = db.getSiblingDB("expression_mod");
    assert.commandWorked(testDB.dropDatabase());
    var coll = testDB.getCollection("test");

    //
    // Confirm different input numeric types are evaluated correctly.
    //

    // Aggregate checking various combinations of number types.
    // The $match portion ensures they are of the correct type as the shell turns the ints back to
    // doubles at the end so we can not check types with assert.
    coll.save({});
    var result = coll.aggregate({
        $project: {
            _id: 0,
            dub_dub: {$mod: [138.5, 3.0]},
            dub_int: {$mod: [138.5, NumberLong(3)]},
            dub_long: {$mod: [138.5, NumberInt(3)]},
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
                                  $match: {
                                      // 1 is NumberDouble
                                      dub_dub: {$type: 1},
                                      dub_int: {$type: 1},
                                      dub_long: {$type: 1},
                                      int_dub: {$type: 1},
                                      // 16 is NumberInt
                                      int_dubint: {$type: 16},
                                      int_int: {$type: 16},
                                      // 18 is NumberLong
                                      int_long: {$type: 18},
                                      long_dub: {$type: 1},
                                      long_dubint: {$type: 18},
                                      long_dublong: {$type: 1},
                                      long_int: {$type: 18},
                                      long_long: {$type: 18},
                                      verylong_verylong: {$type: 18}
                                  }
                                });

    // Correct answers (it is mainly the types that are important here).
    var expectedResult = [{
        dub_dub: 0.5,
        dub_int: 0.5,
        dub_long: 0.5,
        int_dub: 1.5,
        int_dubint: 2,
        int_int: 2,
        int_long: NumberLong(2),
        long_dub: 1.5,
        long_dubint: NumberLong(2),
        long_dublong: 50000000000,
        long_int: NumberLong(2),
        long_long: NumberLong(2),
        verylong_verylong: NumberLong(200000000000)
    }];

    assert.eq(result.toArray(), expectedResult, tojson(result));

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
})();
