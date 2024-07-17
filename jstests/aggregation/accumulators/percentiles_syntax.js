/**
 * Tests for the $percentile accumulator syntax.
 * @tags: [
 *   requires_fcv_81,
 *   # TODO SERVER-91582: Support sharding
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const coll = db[jsTestName()];
coll.drop();
// These tests don't validate the computed $percentile but we need a result to be produced in
// order to check its format.
coll.insert({x: 42});

function assertInvalidSyntax({pSpec, letSpec, errorCode, msg}) {
    let command = {pipeline: [{$group: {_id: null, p: pSpec}}], let : letSpec, cursor: {}};
    if (errorCode) {
        assert.commandFailedWithCode(coll.runCommand("aggregate", command), errorCode, msg);
    } else {
        assert.commandFailed(coll.runCommand("aggregate", command), msg);
    }
}

function assertValidSyntax({pSpec, letSpec, msg}) {
    let command = {pipeline: [{$group: {_id: null, p: pSpec}}], let : letSpec, cursor: {}};
    assert.commandWorked(coll.runCommand("aggregate", command), msg);
}

/**
 * Test missing or unexpected fields in $percentile spec.
 */
assertInvalidSyntax(
    {pSpec: {$percentile: 0.5}, msg: "Should fail if $percentile is not an object"});

assertInvalidSyntax({
    pSpec: {$percentile: {input: "$x", method: "approximate"}},
    msg: "Should fail if $percentile is missing 'p' field"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5], method: "approximate"}},
    msg: "Should fail if $percentile is missing 'input' field"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5], input: "$x"}},
    msg: "Should fail if $percentile is missing 'method' field"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5], input: "$x", method: "approximate", extras: 42}},
    msg: "Should fail if $percentile contains an unexpected field"
});

/**
 * Test invalid 'p' field, specified as a constant.
 */
assertInvalidSyntax({
    pSpec: {$percentile: {p: 0.5, input: "$x", method: "approximate"}},
    msg: "Should fail if 'p' field in $percentile isn't array"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [], input: "$x", method: "approximate"}},
    msg: "Should fail if 'p' field in $percentile is an empty array"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, "foo"], input: "$x", method: "approximate"}},
    msg: "Should fail if 'p' field in $percentile is an array with a non-numeric element"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, 10], input: "$x", method: "approximate"}},
    msg:
        "Should fail if 'p' field in $percentile is an array with any value outside of [0, 1] range"
});

/**
 * Test invalid 'p' field, specified as an expression.
 */
assertInvalidSyntax({
    pSpec: {$percentile: {p: ["$x"], input: "$x", method: "approximate"}},
    msg: "'p' should not accept non-const expressions"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: {$add: [0.1, 0.5]}, input: "$x", method: "approximate"}},
    msg: "'p' should not accept expressions that evaluate to a non-array"
});

assertInvalidSyntax({
    pSpec: {
        $percentile:
            {p: {$concatArrays: [[0.01, 0.1], ["foo"]]}, input: "$x", method: "approximate"}
    },
    msg: "'p' should not accept expressions that evaluate to an array with non-numeric elements"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: "$$pvals", input: "$x", method: "approximate"}},
    letSpec: {pvals: 0.5},
    msg: "'p' should not accept variables that evaluate to a non-array"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: "$$pvals", input: "$x", method: "approximate"}},
    letSpec: {pvals: [0.5, "foo"]},
    msg: "'p' should not accept variables that evaluate to an array with non-numeric elements"
});

/**
 * Test invalid 'method' field.
 */
assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.7], input: "$x", method: 42}},
    msg: "$percentile should fail if 'method' field isn't a string"
});

assertInvalidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.7], input: "$x", method: "fancy"}},
    msg: "$percentile should fail if 'method' isn't one of _predefined_ strings"
});

if (FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    assertValidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: "$x", method: "discrete"}},
        msg: "Should work with discrete 'method'"
    });

    assertInvalidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: "$x", method: "continuous"}},
        errorCode: ErrorCodes.InternalErrorNotSupported,
        msg: "$percentile should fail because continuous 'method' isn't implemented yet"
    });

} else {
    assertInvalidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: "$x", method: "discrete"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$percentile should fail because discrete 'method' isn't supported yet"
    });

    assertInvalidSyntax({
        pSpec: {$percentile: {p: [0.5, 0.7], input: "$x", method: "continuous"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$percentile should fail because continuous 'method' isn't supported yet"
    });
}

/**
 * Tests for invalid $median.
 */
assertInvalidSyntax({
    pSpec: {$median: {p: [0.5], input: "$x", method: "approximate"}},
    msg: "$median should fail if 'p' is defined"
});

assertInvalidSyntax({
    pSpec: {$median: {method: "approximate"}},
    msg: "$median should fail if 'input' field is missing"
});

assertInvalidSyntax(
    {pSpec: {$median: {input: "$x"}}, msg: "Median should fail if 'method' field is missing"});

assertInvalidSyntax({
    pSpec: {$median: {input: "$x", method: "approximate", extras: 42}},
    msg: "$median should fail if there is an unexpected field"
});

assertInvalidSyntax({
    pSpec: {$median: {input: "$x", method: "fancy"}},
    msg: "$median should fail if 'method' isn't one of the _predefined_ strings"
});

if (FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    assertValidSyntax({
        pSpec: {$median: {input: "$x", method: "discrete"}},
        msg: "Should work with discrete 'method'"
    });

    assertInvalidSyntax({
        pSpec: {$median: {input: "$x", method: "continuous"}},
        errorCode: ErrorCodes.InternalErrorNotSupported,
        msg: "$median should fail because continuous 'method' isn't implemented yet"
    });

} else {
    assertInvalidSyntax({
        pSpec: {$median: {input: "$x", method: "discrete"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$median should fail because discrete 'method' isn't supported yet"
    });

    assertInvalidSyntax({
        pSpec: {$median: {input: "$x", method: "continuous"}},
        errorCode: ErrorCodes.BadValue,
        msg: "$median should fail because continuous 'method' isn't supported yet"
    });
}

/**
 * Test that valid $percentile specifications are accepted. The results, i.e. semantics, are tested
 * elsewhere and would cover all of the cases below, we are providing them here nonetheless for
 * completeness.
 */
assertValidSyntax({
    pSpec: {$percentile: {p: [0.0, 0.0001, 0.5, 0.995, 1.0], input: "$x", method: "approximate"}},
    msg: "Should be able to specify an array of percentiles"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.9], input: {$divide: ["$x", 2]}, method: "approximate"}},
    msg: "Should be able to specify 'input' as an expression"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.9], input: "x", method: "approximate"}},
    msg: "Non-numeric inputs should be gracefully ignored"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [0.5, 0.9], input: {$add: [2, "$x"]}, method: "approximate"}},
    msg: "'input' should be able to use expressions"
});

assertValidSyntax({
    pSpec: {
        $percentile: {p: [0.5, 0.9], input: {$concatArrays: [[2], ["$x"]]}, method: "approximate"}
    },
    msg: "'input' should be able to use expressions even if the result of their eval is non-numeric"
});

assertValidSyntax({
    pSpec: {
        $percentile:
            {p: {$concatArrays: [[0.01, 0.1], [0.9, 0.99]]}, input: "$x", method: "approximate"}
    },
    msg: "'p' should be able to use expressions that evaluate to an array"
});

assertValidSyntax({
    pSpec: {$percentile: {p: [{$add: [0.1, 0.5]}], input: "$x", method: "approximate"}},
    msg: "'p' should be able to use expressions for the array elements"
});

assertValidSyntax({
    pSpec: {$percentile: {p: "$$pvals", input: "$x", method: "approximate"}},
    letSpec: {pvals: [0.5, 0.9]},
    msg: "'p' should be able to use variables for the array"
});

assertValidSyntax({
    pSpec: {$percentile: {p: ["$$p1", "$$p2"], input: "$x", method: "approximate"}},
    letSpec: {p1: 0.5, p2: 0.9},
    msg: "'p' should be able to use variables for the array elements"
});

/**
 * Tests for valid $median.
 */
assertValidSyntax(
    {pSpec: {$median: {input: "$x", method: "approximate"}}, msg: "Simple base case for $median."});
