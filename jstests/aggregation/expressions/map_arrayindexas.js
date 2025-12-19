/**
 * Test the behavior of the $map operator with the arrayIndexAs field.
 *
 * @tags: [requires_fcv_83]
 */
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

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

assert.commandWorked(
    coll.insert({
        simple: [1, 2, 3, 4],
        a4z: [0, 0, 0, 0],
        nested: [{a: 1}, {a: 2}],
        mixed: [{a: 1}, {}, {a: 2}, {a: null}],
        notArray: 1,
        null: null,
    }),
);

test({$map: {input: "$mixed", in: "$$IDX"}}, [0, 1, 2, 3]);

test({$map: {input: "$mixed", arrayIndexAs: "σημείο", in: "$$σημείο"}}, [0, 1, 2, 3]);

test(
    {
        $map: {
            input: "$simple",
            arrayIndexAs: "idx",
            as: "this",
            in: {$add: ["$$idx", "$$this"]},
        },
    },
    [1, 3, 5, 7],
);

// Nested behavior with two named index variables.
test(
    {
        $map: {
            input: "$a4z",
            arrayIndexAs: "outer",
            in: {
                $map: {
                    input: "$a4z",
                    arrayIndexAs: "inner",
                    in: {$add: [{$multiply: [4, "$$outer"]}, "$$inner", 1]},
                },
            },
        },
    },
    [
        [1, 2, 3, 4],
        [5, 6, 7, 8],
        [9, 10, 11, 12],
        [13, 14, 15, 16],
    ],
);

// Nested behavior for shadowing variables.
test({$map: {input: "$a4z", in: {$map: {input: "$a4z", in: "$$IDX"}}}}, [
    [0, 1, 2, 3],
    [0, 1, 2, 3],
    [0, 1, 2, 3],
    [0, 1, 2, 3],
]);

// Nested behavior with named inner variable and default outer variable.
test({$map: {input: "$a4z", in: {$map: {input: "$a4z", arrayIndexAs: "inner", in: {$add: ["$$IDX", "$$inner"]}}}}}, [
    [0, 1, 2, 3],
    [1, 2, 3, 4],
    [2, 3, 4, 5],
    [3, 4, 5, 6],
]);

//
// Test error conditions.
//

// Can't use default $$IDX if 'arrayIndexAs' is defined.
testError({$map: {input: "$simple", arrayIndexAs: "index", in: "$$IDX"}}, 17276);

// Can't use non-user definable names on 'arrayIndexAs'.
testError({$map: {input: "$simple", arrayIndexAs: "IDX", in: []}}, ErrorCodes.FailedToParse);
testError({$map: {input: "$simple", arrayIndexAs: "^", in: []}}, ErrorCodes.FailedToParse);
testError({$map: {input: "$simple", arrayIndexAs: "", in: []}}, ErrorCodes.FailedToParse);

// Can't use variable defined by 'arrayIndexAs' or $$IDX in the non-'in' arguments.
testError({$map: {input: "$$IDX", in: {}}}, 17276);
testError({$map: {input: "$$i", arrayIndexAs: "i", in: {}}}, 17276);

// Can't reuse same variable.
testError({$map: {input: "$simple", as: "i", arrayIndexAs: "i", in: "$$i"}}, 9375801);

// Can't use 'arrayIndexAs' in API Version 1 with apiStrict.
testError({$map: {input: "$simple", arrayIndexAs: "i", in: "$$i"}}, ErrorCodes.APIStrictError, {
    apiVersion: "1",
    apiStrict: true,
});
