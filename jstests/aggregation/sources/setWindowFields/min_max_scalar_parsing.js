/**
 * Test that $minMaxScalar window function expression parsing works.
 * @tags: [featureFlagSearchHybridScoringFull, requires_fcv_81]
 */

const coll = db[jsTestName()];
coll.drop();

// Helper functions to assert which $setWindowField args are expected to pass
// and which are expected to fail.
function expectSuccessWithArgs(setWindowFieldsArgs) {
    coll.aggregate([{$setWindowFields: setWindowFieldsArgs}]);
}
function expectFailureWithArgs(setWindowFieldsArgs, errorCode) {
    assert.throwsWithCode(function() {
        coll.aggregate([{$setWindowFields: setWindowFieldsArgs}]);
    }, errorCode);
}

// Test that expected syntax is accepted and unexpected syntax is rejected.
// Does not yet test value of results, just that command runs or not.
//
// $setWindowFields args that should fail (with documented reason).
expectFailureWithArgs(
    // Unexpected input argument to $minMaxScalar.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", hello: "world"},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'input' cannot be missing.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {min: 0, max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'min' must be a numerical type.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", min: "hello", max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'max' must be a numerical type.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", min: 0, max: "world"},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Cannot specify 'min', but not 'max'.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", min: 0},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Cannot specify 'max' but not 'min'.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'max' must be strictly greater than 'min'.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", min: 10, max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'max' must be strictly greater than 'min'.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", min: 11, max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: document based bounds cannot have lower bound greater than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {documents: [1, "unbounded"]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: document based bounds cannot have upper bound less than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {documents: [-2, -1]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: range based bounds cannot have lower bound greater than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {range: [1.5, "unbounded"]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: range based bounds cannot have upper bound less than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScalar: {input: "$x"}, window: {range: [-1.5, -0.5]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Unspecified bounds default to unbounded document bounds, which are currently unsupported.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScalar: {input: "$x", min: 0, max: 10},
            },
        }
    },
    ErrorCodes.NotImplemented);

// $setWindowFields args that should succeed.
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue":
            {$minMaxScalar: {input: "$x"}, window: {documents: ["current", "unbounded"]}},
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScalar: {input: "$x", min: 0, max: 10},
            window: {documents: ["current", "unbounded"]}
        },
    }
});
