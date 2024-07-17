/**
 * Tests for the $percentile expression syntax.
 * @tags: [
 *   requires_fcv_81,
 *   # TODO SERVER-91582: Support sharding
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const coll = db.expression_percentile;
coll.drop();

assert.commandWorked(coll.insert([{_id: 0, k1: 3, k2: 2, k3: "hi", k4: [1, 2, 3]}]));

function assertInvalidSyntax({pSpec, letSpec, errorCode, msg}) {
    let command = {pipeline: [{$project: {p: pSpec}}], let : letSpec, cursor: {}};
    if (errorCode) {
        assert.commandFailedWithCode(coll.runCommand("aggregate", command), errorCode, msg);
    } else {
        assert.commandFailed(coll.runCommand("aggregate", command), msg);
    }
}

function assertValidSyntax({pSpec, letSpec, msg}) {
    let command = {pipeline: [{$project: {p: pSpec}}], let : letSpec, cursor: {}};
    assert.commandWorked(coll.runCommand("aggregate", command), msg);
}

/**
 * Test missing or unexpected fields in $percentile spec.
 */
assertInvalidSyntax(
    {pSpec: {$percentile: 0.5}, msg: "Should fail if $percentile is not an object"});

assertInvalidSyntax({
    pSpec: {$percentile: {input: ["$k1", "$k2"], method: "approximate"}},
    msg: "Should fail if $percentile is missing 'p' field"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5], method: "approximate"}},
    msg: "Should fail if $percentile is missing 'input' field"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5], input: "$k1"}},
    msg: "Should fail if $percentile is missing 'method' field"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5], input: ["$k1", "$k2"], method: "approximate", extras: 42}},
    msg: "Should fail if $percentile contains an unexpected field"
});

/**
 * Test invalid 'p' field, specified as a constant.
 */
assertInvalidSyntax({
    pSpec: {$percentile: {p: 0.5, input: ["$k1", "$k2"], method: "approximate"}},
    msg: "Should fail if 'p' field in $percentile isn't array"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [], input: ["$k1", "$k2"], method: "approximate"}},
    msg: "Should fail if 'p' field in $percentile is an empty array"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, "foo"], input: ["$k1", "$k2"], method: "approximate"}},
    msg: "Should fail if 'p' field in $percentile is an array with a non-numeric element"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, 10], input: ["$k1", "$k2"], method: "approximate"}},
    msg:
        "Should fail if 'p' field in $percentile is an array with any value outside of [0, 1] range"
});

/**
 * Test invalid 'p' field, specified as an expression.
 */
assertInvalidSyntax({
    pSpec: {$percentile: {p: ["$x"], input: ["$k1", "$k2"], method: "approximate"}},
    msg: "'p' should not accept non-const expressions"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: {$add: [0.1, 0.5]}, input: ["$k1", "$k2"], method: "approximate"}},
    msg: "'p' should not accept expressions that evaluate to a non-array"
});

assertInvalidSyntax({
    pSpec: {
        $percentile: {
            p: {$concatArrays: [[0.01, 0.1], ["foo"]]},
            input: ["$k1", "$k2"],
            method: "approximate"
        }
    },
    msg: "'p' should not accept expressions that evaluate to an array with non-numeric elements"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: "$$pvals", input: ["$k1", "$k2"], method: "approximate"}},
    letSpec: {pvals: 0.5},
    msg: "'p' should not accept variables that evaluate to a non-array"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: "$$pvals", input: ["$k1", "$k2"], method: "approximate"}},
    letSpec: {pvals: [0.5, "foo"]},
    msg: "'p' should not accept variables that evaluate to an array with non-numeric elements"
});

/**
 * Test invalid 'method' field.
 */
assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], method: 42}},
    msg: "$percentile should fail if 'method' field isn't a string"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], method: "fancy"}},
    msg: "$percentile should fail if 'method' isn't one of the _predefined_ strings"
});

if (FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    assertValidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], method: "discrete"}},
        msg: "Should work with discrete 'method'"
    });

    assertInvalidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], method: "continuous"}},
        errorCode: ErrorCodes.InternalErrorNotSupported,
        msg: "$percentile should fail because continuous 'method' isn't implemented yet"
    });

} else {
    assertInvalidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], method: "discrete"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$percentile should fail because discrete 'method' isn't supported yet"
    });

    assertInvalidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], method: "continuous"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$percentile should fail because continuous 'method' isn't supported yet"
    });
}

/**
 * Tests for $median.
 */
assertInvalidSyntax({
    pSpec: {$median: {p: [0.5], input: "$k4", method: "approximate"}},
    msg: "Should fail if 'p' is defined"
});

assertInvalidSyntax({
    pSpec: {$median: {method: "approximate"}},
    msg: "Should fail if $median is missing 'input' field"
});

assertInvalidSyntax({
    pSpec: {$median: {input: ["$k1", "$k2"]}},
    msg: "Should fail if $median is missing 'method' field"
});

assertInvalidSyntax({
    pSpec: {$median: {input: "$x", method: "approximate", extras: 42}},
    msg: "Should fail if $median contains an unexpected field"
});

assertInvalidSyntax({
    pSpec: {$median: {input: ["$k1", "$k2"], method: "fancy"}},
    msg: "$median should fail if 'method' isn't one of the _predefined_ strings"
});

if (FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    assertValidSyntax({
        pSpec: {$median: {input: ["$k1", "$k2"], method: "discrete"}},
        msg: "Should work with discrete 'method'"
    });

    assertInvalidSyntax({
        pSpec: {$median: {input: ["$k1", "$k2"], method: "continuous"}},
        errorCode: ErrorCodes.InternalErrorNotSupported,
        msg: "$median should fail because continuous 'method' isn't implemented yet"
    });

} else {
    assertInvalidSyntax({
        pSpec: {$median: {input: ["$k1", "$k2"], method: "discrete"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$median should fail because discrete 'method' isn't supported yet"
    });

    assertInvalidSyntax({
        pSpec: {$median: {input: ["$k1", "$k2"], method: "continuous"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$median should fail because continuous 'method' isn't supported yet"
    });
}

/**
 * Test that valid $percentile specifications are accepted. The results, i.e. semantics, are
 * tested elsewhere and would cover all of the cases below, we are providing them here
 * nonetheless for completeness.
 */
assertValidSyntax({
    pSpec: {
        $percentile:
            {p: [0.0, 0.0001, 0.5, 0.995, 1.0], input: ["$k1", "$k2"], method: "approximate"}
    },
    msg: "Should be able to specify an array of percentiles"
});

/**
 * Test valid 'input' field (even if they don't make sense).
 */
assertValidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.9], input: "something", method: "approximate"}},
    msg: "Non-array 'input' field should be gracefully ignored"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [0.5], input: [], method: "approximate"}},
    msg: "Empty array in the 'input' should be ignored"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.9], input: ["k3"], method: "approximate"}},
    msg: "Non-numeric expressions in the 'input' array should be gracefully ignored"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [0.5], input: "$k4", method: "approximate"}},
    msg: "Should work if 'input' field in $percentile is a simple expression"
});

assertValidSyntax({
    pSpec: {
        $percentile: {
            p: [0.5],
            input: {$concatArrays: [["$k1", "$k2"], [{$add: [2, "$k1"]}], "$k4"]},
            method: "approximate"
        }
    },
    msg: "Should work if 'input' field in $percentile is a complex expression"
});

/**
 * Tests for $median.
 */
assertValidSyntax({
    pSpec: {$median: {input: "$k4", method: "approximate"}},
    msg: "Simple base case for $median with single expression input field"
});

assertValidSyntax({
    pSpec: {$median: {input: ["$k1", "$k2"], method: "approximate"}},
    msg: "Simple base case for $median with array input field"
});
