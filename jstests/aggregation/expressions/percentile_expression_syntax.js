/**
 * Tests for the $percentile expression syntax.
 * @tags: [
 *   featureFlagApproxPercentiles
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const coll = db.expression_percentile;
coll.drop();

assert.commandWorked(coll.insert([{_id: 0, k1: 3, k2: 2, k3: "hi", k4: [1, 2, 3]}]));

/**
 * Tests to check that invalid $percentile specifications are rejected.
 */
function assertInvalidSyntax(percentileSpec, msg) {
    assert.commandFailed(
        coll.runCommand("aggregate", {pipeline: [{$project: {p: percentileSpec}}], cursor: {}}),
        msg);
}

assertInvalidSyntax({$percentile: 0.5}, "Should fail if $percentile is not an object");

assertInvalidSyntax({$percentile: {input: ["$k1", "$k2"], algorithm: "approximate"}},
                    "Should fail if $percentile is missing 'p' field");

assertInvalidSyntax({$percentile: {p: [0.5], algorithm: "approximate"}},
                    "Should fail if $percentile is missing 'input' field");

assertInvalidSyntax({$percentile: {p: [0.5], input: "$k1"}},
                    "Should fail if $percentile is missing 'algorithm' field");

assertInvalidSyntax(
    {$percentile: {p: [0.5], input: ["$k1", "$k2"], algorithm: "approximate", extras: 42}},
    "Should fail if $percentile contains an unexpected field");

assertInvalidSyntax({$percentile: {p: 0.5, input: ["$k1", "$k2"], algorithm: "approximate"}},
                    "Should fail if 'p' field in $percentile isn't array");

assertInvalidSyntax({$percentile: {p: [], input: ["$k1", "$k2"], algorithm: "approximate"}},
                    "Should fail if 'p' field in $percentile is an empty array");

assertInvalidSyntax({$percentile: {p: [0.5], input: [], algorithm: "approximate"}},
                    "Should fail if 'input' field in $percentile is an empty array");

assertInvalidSyntax(
    {$percentile: {p: [0.5, "foo"], input: ["$k1", "$k2"], algorithm: "approximate"}},
    "Should fail if 'p' field in $percentile is an array with a non-numeric element");

assertInvalidSyntax(
    {$percentile: {p: [0.5, 10], input: ["$k1", "$k2"], algorithm: "approximate"}},
    "Should fail if 'p' field in $percentile is an array with any value outside of [0, 1] range");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], algorithm: 42}},
                    "Should fail if 'algorithm' field isn't a string");

assertInvalidSyntax({$percentile: {p: [0.5, 0.7], input: ["$k1", "$k2"], algorithm: "fancy"}},
                    "Should fail if 'algorithm' isn't one of _predefined_ strings");

/**
 * Tests for $median. $median desugars to $percentile with the field p:[0.5] added, and therefore
 * has similar syntax to $percentile.
 */

assertInvalidSyntax({$median: {p: [0.5], input: "$k4", algorithm: "approximate"}},
                    "Should fail if 'p' is defined");

assertInvalidSyntax({$median: {algorithm: "approximate"}},
                    "Should fail if $median is missing 'input' field");

assertInvalidSyntax({$median: {input: [], algorithm: "approximate"}},
                    "Should fail if $median has an empty array as its 'input' field");

assertInvalidSyntax({$median: {input: ["$k1", "$k2"]}},
                    "Should fail if $median is missing 'algorithm' field");

assertInvalidSyntax({$median: {input: "$x", algorithm: "approximate", extras: 42}},
                    "Should fail if $median contains an unexpected field");

/**
 * Test that valid $percentile specifications are accepted. The results, i.e. semantics, are
 * tested elsewhere and would cover all of the cases below, we are providing them here
 * nonetheless for completeness.
 */
function assertValidSyntax(percentileSpec, msg) {
    assert.commandWorked(
        coll.runCommand("aggregate", {pipeline: [{$project: {p: percentileSpec}}], cursor: {}}),
        msg);
}

assertValidSyntax(
    {$percentile: {p: [0.0, 0.0001, 0.5, 0.995, 1.0], input: ["$k1"], algorithm: "approximate"}},
    "Should be able to specify an array of percentiles");

assertValidSyntax({$percentile: {p: [0.5, 0.9], input: ["k3"], algorithm: "approximate"}},
                  "Non-numeric expressions in input array should be gracefully ignored");

assertValidSyntax({$percentile: {p: [0.5], input: "$k4", algorithm: "approximate"}},
                  "Should work if 'input' field in $percentile is a single expression");

/**
 * Tests for $median. $median desugars to $percentile with the field p:[0.5] added.
 */

assertValidSyntax({$median: {input: "$k4", algorithm: "approximate"}},
                  "Simple base case for $median with single expression input field");

assertValidSyntax({$median: {input: ["$k1", "$k2"], algorithm: "approximate"}},
                  "Simple base case for $median with array input field");
})();
