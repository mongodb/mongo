/**
 * Test the behavior of the $reduce operator with the arrayIndexAs field.
 *
 * @tags: [requires_fcv_83]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

function testError(expression, code, options = {}) {
    const projectSpec = {$project: {b: expression}};
    assertErrorCode(coll, projectSpec, code, "", options);
}

function test(expression, expected) {
    let result = coll.aggregate({$project: {_id: 0, res: expression}}).toArray();
    assert.eq(result, [{res: expected}]);
}

let coll = db[jsTestName()];
coll.drop();

if (FeatureFlagUtil.isPresentAndEnabled(db, "ExposeArrayIndexInMapFilterReduce")) {
    assert.commandWorked(
        coll.insert({
            _id: 0,
            simple: [1, 2, 3],
            matrix: [
                [1, 2, 3],
                [4, 5, 6],
                [7, 8, 9],
            ],
        }),
    );

    test({$reduce: {input: "$simple", initialValue: 0, in: {$add: [{$multiply: ["$$this", "$$IDX"]}, "$$value"]}}}, 8);

    test(
        {
            $reduce: {
                input: "$simple",
                initialValue: 0,
                arrayIndexAs: "i",
                in: {$add: [{$multiply: ["$$this", "$$i"]}, "$$value"]},
            },
        },
        8,
    );

    let pipeline = {
        $reduce: {
            input: "$matrix",
            initialValue: 1,
            in: {
                $multiply: [
                    "$$value",
                    {
                        $reduce: {
                            input: "$$this",
                            initialValue: 0,
                            in: {$add: ["$$value", "$$this", "$$IDX"]},
                        },
                    },
                ],
            },
        },
    };
    test(pipeline, 4374);

    //
    // Test error conditions.
    //

    // Can't use default $$IDX if 'arrayIndexAs' is defined.
    pipeline = {
        $reduce: {
            input: "$simple",
            initialValue: 0,
            arrayIndexAs: "i",
            in: {$add: ["$$this", "$$IDX", "$$value"]},
        },
    };
    testError(pipeline, 17276);

    // Can't use non-user definable names on 'arrayIndexAs'.
    testError({$reduce: {input: "$simple", arrayIndexAs: "IDX", initialValue: [], in: []}}, ErrorCodes.FailedToParse);
    testError({$reduce: {input: "$simple", arrayIndexAs: "^", initialValue: [], in: []}}, ErrorCodes.FailedToParse);
    testError({$reduce: {input: "$simple", arrayIndexAs: "", initialValue: [], in: []}}, ErrorCodes.FailedToParse);

    // Can't use variable defined by 'arrayIndexAs' or $$IDX in the non-'in' arguments.
    testError({$reduce: {input: "$$i", initialValue: [], in: [], arrayIndexAs: "i"}}, 17276);
    testError({$reduce: {input: "$simple", initialValue: "$$i", in: [], arrayIndexAs: "i"}}, 17276);
    testError({$reduce: {input: "$simple", initialValue: ["$$i"], in: [], arrayIndexAs: "i"}}, 17276);
    testError({$reduce: {input: "$$IDX", initialValue: [], in: []}}, 17276);
    testError({$reduce: {input: "$simple", initialValue: "$$IDX", in: []}}, 17276);
    testError({$reduce: {input: "$simple", initialValue: ["$$IDX"], in: []}}, 17276);

    // Can't use 'arrayIndexAs' in API Version 1 with apiStrict.
    pipeline = {
        $reduce: {
            input: "$simple",
            arrayIndexAs: "i",
            in: {
                $add: ["$$this", "$$value", "$$i"],
            },
        },
    };
    testError(pipeline, ErrorCodes.APIStrictError, {
        apiVersion: "1",
        apiStrict: true,
    });
} else {
    // TODO(SERVER-90514): remove these tests when the new features are enabled by default.
    testError(
        {$reduce: {input: "$simple", initialValue: 0, in: {$add: [{$multiply: ["$$this", "$$IDX"]}, "$$value"]}}},
        17276,
    );
    testError(
        {
            $reduce: {
                input: "$simple",
                initialValue: 0,
                arrayIndexAs: "i",
                in: {$add: [{$multiply: ["$$this", "$$i"]}, "$$value"]},
            },
        },
        40076,
    );
}
