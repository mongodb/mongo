// In SERVER-17258, the $reduce expression was introduced. In this test file, we check the
// functionality and error cases of the expression.
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode and testExpression.

(function() {
    "use strict";

    var coll = db.reduce;

    testExpression(
        coll,
        {
          $reduce:
              {input: [1, 2, 3], initialValue: {$literal: 0}, in : {$sum: ["$$this", "$$value"]}}
        },
        6);
    testExpression(coll, {$reduce: {input: [], initialValue: {$literal: 0}, in : 10}}, 0);
    testExpression(
        coll,
        {
          $reduce:
              {input: [1, 2, 3], initialValue: [], in : {$concatArrays: ["$$value", ["$$this"]]}}
        },
        [1, 2, 3]);
    testExpression(coll,
                   {
                     $reduce: {
                         input: [1, 2],
                         initialValue: [],
                         in : {$concatArrays: ["$$value", ["$$value.notAField"]]}
                     }
                   },
                   [[], []]);

    // A nested $reduce which sums each subarray, then multiplies the results.
    testExpression(coll,
                   {
                     $reduce: {
                         input: [[1, 2, 3], [4, 5]],
                         initialValue: 1,
                         in : {
                             $multiply: [
                                 "$$value",
                                 {
                                   $reduce: {
                                       input: "$$this",
                                       initialValue: 0,
                                       in : {$sum: ["$$value", "$$this"]}
                                   }
                                 }
                             ]
                         }
                     }
                   },
                   54);

    // A nested $reduce using a $let to allow the inner $reduce to access the variables of the
    // outer.
    testExpression(coll,
                   {
                     $reduce: {
                         input: [[0, 1], [2, 3]],
                         initialValue: {allElements: [], sumOfInner: {$literal: 0}},
                         in : {
                             $let: {
                                 vars: {outerValue: "$$value", innerArray: "$$this"},
                                 in : {
                                     $reduce: {
                                         input: "$$innerArray",
                                         initialValue: "$$outerValue",
                                         in : {
                                             allElements: {
                                                 $concatArrays:
                                                     ["$$value.allElements", ["$$this"]]
                                             },
                                             sumOfInner:
                                                 {$sum: ["$$value.sumOfInner", "$$this"]}
                                         }
                                     }
                                 }
                             }
                         }
                     }
                   },
                   {allElements: [0, 1, 2, 3], sumOfInner: 6});

    // Nullish input produces null as an output.
    testExpression(coll, {$reduce: {input: null, initialValue: {$literal: 0}, in : 5}}, null);
    testExpression(
        coll, {$reduce: {input: "$nonexistent", initialValue: {$literal: 0}, in : 5}}, null);

    // Error cases for $reduce.

    // $reduce requires an object.
    var pipeline = {$project: {reduced: {$reduce: 0}}};
    assertErrorCode(coll, pipeline, 40075);

    // Unknown field specified.
    pipeline = {
        $project: {
            reduced: {
                $reduce: {
                    input: {$literal: 0},
                    initialValue: {$literal: 0},
                    in : {$literal: 0},
                    notAField: {$literal: 0}
                }
            }
        }
    };
    assertErrorCode(coll, pipeline, 40076);

    // $reduce requires input to be specified.
    pipeline = {$project: {reduced: {$reduce: {initialValue: {$literal: 0}, in : {$literal: 0}}}}};
    assertErrorCode(coll, pipeline, 40077);

    // $reduce requires initialValue to be specified.
    pipeline = {$project: {reduced: {$reduce: {input: {$literal: 0}, in : {$literal: 0}}}}};
    assertErrorCode(coll, pipeline, 40078);

    // $reduce requires in to be specified.
    pipeline = {
        $project: {reduced: {$reduce: {input: {$literal: 0}, initialValue: {$literal: 0}}}}
    };
    assertErrorCode(coll, pipeline, 40079);

    // $$value is undefined in the non-'in' arguments of $reduce.
    pipeline = {$project: {reduced: {$reduce: {input: "$$value", initialValue: [], in : []}}}};
    assertErrorCode(coll, pipeline, 17276);

    // $$this is undefined in the non-'in' arguments of $reduce.
    pipeline = {$project: {reduced: {$reduce: {input: "$$this", initialValue: [], in : []}}}};
    assertErrorCode(coll, pipeline, 17276);
}());
