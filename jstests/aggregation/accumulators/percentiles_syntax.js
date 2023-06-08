/**
 * Tests for the $percentile accumulator syntax.
 * @tags: [
 *   requires_fcv_70,
 *   featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db[jsTestName()];
coll.drop();
// These tests don't validate the computed $percentile but we need a result to be produced in
// order to check its format.
coll.insert({x: 42});

/**
 * Tests to check that invalid $percentile specifications are gracefully rejected.
 */
function assertInvalidSyntax(percentileSpec, msg) {
    assert.commandFailed(
        coll.runCommand("aggregate",
                        {pipeline: [{$group: {_id: null, p: percentileSpec}}], cursor: {}}),
        msg);
}

assertInvalidSyntax({$percentile: 0.5}, "Should fail if $percentile is not an object");

assertInvalidSyntax({$percentile: {input: "$x", method: "approximate"}},
                    "Should fail if $percentile is missing 'p' field");

assertInvalidSyntax({$percentile: {p: [0.5], method: "approximate"}},
                    "Should fail if $percentile is missing 'input' field");

assertInvalidSyntax({$percentile: {p: [0.5], input: "$x"}},
                    "Should fail if $percentile is missing 'method' field");

assertInvalidSyntax({$percentile: {p: [0.5], input: "$x", method: "approximate", extras: 42}},
                    "Should fail if $percentile contains an unexpected field");

assertInvalidSyntax({$percentile: {p: 0.5, input: "$x", method: "approximate"}},
                    "Should fail if 'p' field in $percentile isn't array");

assertInvalidSyntax({$percentile: {p: [], input: "$x", method: "approximate"}},
                    "Should fail if 'p' field in $percentile is an empty array");

assertInvalidSyntax(
    {$percentile: {p: [0.5, "foo"], input: "$x", method: "approximate"}},
    "Should fail if 'p' field in $percentile is an array with a non-numeric element");

assertInvalidSyntax(
    {$percentile: {p: [0.5, 10], input: "$x", method: "approximate"}},
    "Should fail if 'p' field in $percentile is an array with any value outside of [0, 1] range");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: "$x", method: 42}},
                    "$percentile should fail if 'method' field isn't a string");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: "$x", method: "fancy"}},
                    "$percentile should fail if 'method' isn't one of _predefined_ strings");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: "$x", method: "discrete"}},
                    "$percentile should fail because discrete 'method' isn't supported yet");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: "$x", method: "continuous"}},
                    "$percentile should fail because continuous 'method' isn't supported yet");

/**
 * Tests for $median.
 */
assertInvalidSyntax({$median: {p: [0.5], input: "$x", method: "approximate"}},
                    "$median should fail if 'p' is defined");

assertInvalidSyntax({$median: {method: "approximate"}},
                    "$median should fail if 'input' field is missing");

assertInvalidSyntax({$median: {input: "$x"}}, "Median should fail if 'method' field is missing");

assertInvalidSyntax({$median: {input: "$x", method: "approximate", extras: 42}},
                    "$median should fail if there is an unexpected field");

assertInvalidSyntax({$median: {input: "$x", method: "fancy"}},
                    "$median should fail if 'method' isn't one of the _predefined_ strings");

assertInvalidSyntax({$median: {input: "$x", method: "discrete"}},
                    "$median should fail because discrete 'method' isn't supported yet");

assertInvalidSyntax({$median: {input: "$x", method: "continuous"}},
                    "$median should fail because continuous 'method' isn't supported yet");

/**
 * Test that valid $percentile specifications are accepted. The results, i.e. semantics, are tested
 * elsewhere and would cover all of the cases below, we are providing them here nonetheless for
 * completeness.
 */
function assertValidSyntax(percentileSpec, msg) {
    assert.commandWorked(
        coll.runCommand("aggregate",
                        {pipeline: [{$group: {_id: null, p: percentileSpec}}], cursor: {}}),
        msg);
}

assertValidSyntax(
    {$percentile: {p: [0.0, 0.0001, 0.5, 0.995, 1.0], input: "$x", method: "approximate"}},
    "Should be able to specify an array of percentiles");

assertValidSyntax(
    {$percentile: {p: [0.5, 0.9], input: {$divide: ["$x", 2]}, method: "approximate"}},
    "Should be able to specify 'input' as an expression");

assertValidSyntax({$percentile: {p: [0.5, 0.9], input: "x", method: "approximate"}},
                  "Non-numeric inputs should be gracefully ignored");

/**
 * Tests for $median. $median desugars to $percentile with the field p:[0.5] added.
 */

assertValidSyntax({$median: {input: "$x", method: "approximate"}}, "Simple base case for $median.");
})();
