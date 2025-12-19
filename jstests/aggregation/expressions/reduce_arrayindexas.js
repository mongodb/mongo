/**
 * Test the behavior of the $reduce operator with the arrayIndexAs field.
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

//
// Test 'as' and 'asValue'.
//

test({$reduce: {input: "$simple", initialValue: 0, in: {$add: ["$$this", "$$value"]}}}, 6);
test({$reduce: {input: "$simple", initialValue: 0, as: "elem", in: {$add: ["$$elem", "$$value"]}}}, 6);
test({$reduce: {input: "$simple", initialValue: 0, as: "elem", valueAs: "acc", in: {$add: ["$$elem", "$$acc"]}}}, 6);
test({$reduce: {input: "$simple", initialValue: 0, valueAs: "acc", in: {$add: ["$$this", "$$acc"]}}}, 6);

// Check nested operators.
pipeline = {
    $reduce: {
        input: "$matrix",
        initialValue: 1,
        as: "elem1",
        valueAs: "acc1",
        in: {
            $multiply: [
                "$$acc1",
                {
                    $reduce: {
                        input: "$$elem1",
                        initialValue: 0,
                        as: "elem2",
                        valueAs: "acc2",
                        in: {$add: ["$$acc2", "$$elem2", "$$IDX"]},
                    },
                },
            ],
        },
    },
};
test(pipeline, 4374);

// Check nested variable shadowing.
pipeline = {
    $reduce: {
        input: "$matrix",
        initialValue: 1,
        as: "elem",
        valueAs: "acc",
        in: {
            $multiply: [
                "$$acc",
                {
                    $reduce: {
                        input: "$$elem",
                        initialValue: 0,
                        as: "elem",
                        valueAs: "acc",
                        in: {$add: ["$$acc", "$$elem", "$$IDX"]},
                    },
                },
            ],
        },
    },
};
test(pipeline, 4374);

//
// Test error conditions of 'as' and 'valueAs'.
//

// Can't use defaults $$this/$$value if the new parameters are defined.
testError({$reduce: {input: "$simple", initialValue: 0, as: "elem", in: {$add: ["$$this", "$$value"]}}}, 17276);
testError({$reduce: {input: "$simple", initialValue: 0, valueAs: "elem", in: {$add: ["$$this", "$$value"]}}}, 17276);

// Can't use non-user definable names on new parameters.
testError(
    {$reduce: {input: "$simple", initialValue: 0, as: "THIS", in: {$add: ["$$THIS", "$$value"]}}},
    ErrorCodes.FailedToParse,
);
testError(
    {$reduce: {input: "$simple", initialValue: 0, as: "^", in: {$add: ["$$^", "$$value"]}}},
    ErrorCodes.FailedToParse,
);
testError(
    {$reduce: {input: "$simple", initialValue: 0, valueAs: "VALUE", in: {$add: ["$$this", "$$VALUE"]}}},
    ErrorCodes.FailedToParse,
);
testError(
    {$reduce: {input: "$simple", initialValue: 0, valueAs: "^", in: {$add: ["$$this", "$$^"]}}},
    ErrorCodes.FailedToParse,
);

// Can't use defined variables in the non-'in' arguments.
testError({$reduce: {input: "$$i", initialValue: [], in: [], as: "i"}}, 17276);
testError({$reduce: {input: "$simple", initialValue: "$$i", in: [], as: "i"}}, 17276);
testError({$reduce: {input: "$simple", initialValue: ["$$i"], in: [], as: "i"}}, 17276);
testError({$reduce: {input: "$$i", initialValue: [], in: [], valueAs: "i"}}, 17276);
testError({$reduce: {input: "$simple", initialValue: "$$i", in: [], valueAs: "i"}}, 17276);
testError({$reduce: {input: "$simple", initialValue: ["$$i"], in: [], valueAs: "i"}}, 17276);

// Can't reuse same variable.
testError(
    {$reduce: {input: "$simple", initialValue: 0, as: "elem", valueAs: "elem", in: {$add: ["$$elem", "$$elem"]}}},
    9298401,
);
testError(
    {
        $reduce: {
            input: "$simple",
            initialValue: 0,
            as: "elem",
            arrayIndexAs: "elem",
            in: {$add: ["$$elem", "$$elem"]},
        },
    },
    9298401,
);
testError(
    {
        $reduce: {
            input: "$simple",
            initialValue: 0,
            valueAs: "elem",
            arrayIndexAs: "elem",
            in: {$add: ["$$elem", "$$elem"]},
        },
    },
    9298401,
);

// Can't use new parameters in API Version 1 with apiStrict.
pipeline = {$reduce: {input: "$simple", initialValue: 0, as: "elem", in: {$add: ["$$elem", "$$value"]}}};
testError(pipeline, ErrorCodes.APIStrictError, {
    apiVersion: "1",
    apiStrict: true,
});
pipeline = {$reduce: {input: "$simple", initialValue: 0, valueAs: "acc", in: {$add: ["$$this", "$$acc"]}}};
testError(pipeline, ErrorCodes.APIStrictError, {
    apiVersion: "1",
    apiStrict: true,
});
