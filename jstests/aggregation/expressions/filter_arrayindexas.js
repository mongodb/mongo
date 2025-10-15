/**
 * Test the behavior of the $filter operator with the arrayIndexAs field.
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
            b: [0, 7, 2, 5, 4, 0],
            nested: [[], [0, 1], [2, 3], [4, 5]],
        }),
    );

    let filterDoc = {$filter: {input: "$b", cond: {$eq: ["$$this", "$$IDX"]}}};
    test(filterDoc, [0, 2, 4]);

    filterDoc = {$filter: {input: "$b", arrayIndexAs: "i", cond: {$eq: ["$$this", "$$i"]}}};
    test(filterDoc, [0, 2, 4]);

    filterDoc = {$filter: {input: "$b", cond: {$eq: ["$$this", "$$IDX"]}, limit: 2}};
    test(filterDoc, [0, 2]);

    filterDoc = {$filter: {input: "$b", arrayIndexAs: "i", cond: {$eq: ["$$this", "$$i"]}, limit: 1}};
    test(filterDoc, [0]);

    // Nested behavior with two named index variables.
    test(
        {
            $filter: {
                input: "$nested",
                as: "outer",
                arrayIndexAs: "i",
                cond: {
                    $ne: [
                        0,
                        {
                            $size: {
                                $filter: {
                                    input: "$$outer",
                                    as: "inner",
                                    cond: {$eq: ["$$i", "$$inner"]},
                                },
                            },
                        },
                    ],
                },
            },
        },
        [
            [0, 1],
            [2, 3],
        ],
    );

    // Nested behavior for shadowing variables.
    test(
        {
            $filter: {
                input: "$nested",
                as: "outer",
                cond: {
                    $ne: [
                        0,
                        {
                            $size: {
                                $filter: {
                                    input: "$$outer",
                                    as: "inner",
                                    cond: {$eq: ["$$IDX", "$$inner"]},
                                },
                            },
                        },
                    ],
                },
            },
        },
        [[0, 1]],
    );

    // Nested behavior with named inner variable and default outer variable.
    test(
        {
            $filter: {
                input: "$nested",
                as: "outer",
                cond: {
                    $ne: [
                        0,
                        {
                            $size: {
                                $filter: {
                                    input: "$$outer",
                                    as: "inner",
                                    arrayIndexAs: "i",
                                    cond: {
                                        $and: [{$eq: ["$$inner", "$$IDX"]}, {$eq: ["$$inner", "$$i"]}],
                                    },
                                },
                            },
                        },
                    ],
                },
                limit: 2,
            },
        },
        [[0, 1]],
    );

    //
    // Test error conditions.
    //

    // Can't use default $$IDX if 'arrayIndexAs' is defined.
    testError({$filter: {input: "$b", arrayIndexAs: "i", cond: {$eq: ["$$this", "$$IDX"]}}}, 17276);

    // Can't use non-user definable names on 'arrayIndexAs'.
    testError(
        {$filter: {input: "$b", arrayIndexAs: "IDX", cond: {$eq: ["$$this", "$$IDX"]}}},
        ErrorCodes.FailedToParse,
    );
    testError({$filter: {input: "$b", arrayIndexAs: "^", cond: {$eq: ["$$this", "$$^"]}}}, ErrorCodes.FailedToParse);
    testError({$filter: {input: "$b", arrayIndexAs: "", cond: {$eq: ["$$this", "$$IDX"]}}}, ErrorCodes.FailedToParse);

    // Can't use variable defined by 'arrayIndexAs' or $$IDX in the non-'cond' arguments.
    testError({$filter: {input: "$$i", as: "c", cond: true, limit: 1, arrayIndexAs: "i"}}, 17276);
    testError({$filter: {input: "$b", as: "c", cond: true, limit: "$$i", arrayIndexAs: "i"}}, 17276);
    testError({$filter: {input: "$$IDX", as: "c", cond: true, limit: 1}}, 17276);
    testError({$filter: {input: "$b", as: "c", cond: true, limit: "$$IDX"}}, 17276);

    // Can't reuse same variable.
    testError({$filter: {input: "$b", as: "i", cond: true, arrayIndexAs: "i"}}, 9375802);

    // Can't use 'arrayIndexAs' in API Version 1 with apiStrict.
    filterDoc = {$filter: {input: "$b", arrayIndexAs: "i", cond: {$eq: ["$$this", "$$IDX"]}}};
    testError(filterDoc, ErrorCodes.APIStrictError, {
        apiVersion: "1",
        apiStrict: true,
    });
} else {
    // TODO(SERVER-90514): remove these tests when the new features are enabled by default.
    testError({$filter: {input: "$b", cond: {$eq: ["$$this", "$$IDX"]}}}, 17276);
    testError({$filter: {input: "$b", arrayIndexAs: "i", cond: {$eq: ["$$this", "$$i"]}}}, 28647);
}
