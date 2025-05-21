/**
 * Test that $minMaxScaler window function expression parsing works.
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
    // Unexpected input argument to $minMaxScaler.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", hello: "world"},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.IDLUnknownField);
expectFailureWithArgs(
    // 'input' cannot be missing.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {min: 0, max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.IDLFailedToParse);
expectFailureWithArgs(
    // 'min' must be a numerical type.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", min: "hello", max: 10},
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
                $minMaxScaler: {input: "$x", min: 0, max: "world"},
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
                $minMaxScaler: {input: "$x", min: 0},
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
                $minMaxScaler: {input: "$x", max: 10},
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
                $minMaxScaler: {input: "$x", min: 10, max: 10},
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
                $minMaxScaler: {input: "$x", min: 11, max: 10},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'max' must be strictly greater than 'min', even as expressions.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", min: {$multiply: [1, 20]}, max: {$divide: [20, 2]}},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'min' must be a constant
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", min: {$multiply: [1, "$x"]}, max: {$divide: [20, 2]}},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'max' must be a constant
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", min: {$multiply: [1, 1]}, max: {$divide: [20, "$x"]}},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'min' must be numeric
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", min: "hello", max: 2},
                window: {documents: ["current", "unbounded"]},
            },
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // 'max' must be numeric
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {
                $minMaxScaler: {input: "$x", min: 1, max: "hi"},
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
            "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {documents: [1, "unbounded"]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: document based bounds cannot have upper bound less than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {documents: [-2, -1]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: document based bounds cannot have upper bound less than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue":
                {$minMaxScaler: {input: "$x"}, window: {documents: ["unbounded", -1]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: range based bounds cannot have lower bound greater than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: [1.5, "unbounded"]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: range based bounds cannot have upper bound less than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: [-1.5, -0.5]}},
        }
    },
    ErrorCodes.FailedToParse);
expectFailureWithArgs(
    // Inclusivity bounds check: range based bounds cannot have upper bound less than 0.
    {
        sortBy: {_id: 1},
        output: {
            "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", -0.5]}},
        }
    },
    ErrorCodes.FailedToParse);

// $setWindowFields args that should succeed.
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue":
            {$minMaxScaler: {input: "$x"}, window: {documents: ["current", "unbounded"]}},
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: [0, "unbounded"]}},
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {$minMaxScaler: {input: "$x"}, window: {range: ["unbounded", 0]}},
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {input: "$x", min: 0, max: 10},
            window: {documents: ["current", "unbounded"]}
        },
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue":
            {$minMaxScaler: {input: "$x", min: 0, max: 10}, window: {range: [0, "unbounded"]}},
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {input: "$x"},
        },
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {input: "$x", min: 0, max: 10},
        },
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {input: "$x", min: 0, max: 10},
            window: {documents: ["unbounded", "unbounded"]}
        },
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {input: "$x", min: 0, max: 10},
            window: {range: ["unbounded", "unbounded"]}
        },
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {input: "$x", min: {$multiply: [5, 0]}, max: {$subtract: [5, 4]}},
            window: {range: ["unbounded", "unbounded"]}
        },
    }
});
expectSuccessWithArgs({
    sortBy: {_id: 1},
    output: {
        "relativeXValue": {
            $minMaxScaler: {
                input: {$multiply: ["$x", "$x"]},
                min: {$multiply: [5, 0]},
                max: {$subtract: [5, 4]}
            },
            window: {range: ["unbounded", "unbounded"]}
        },
    }
});
