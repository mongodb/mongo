/**
 * Tests for the $_internalAssertDataAssumptions stage. This internal stage validates that specified
 * field paths do not contain arrays, and is used to test the dependency graph's arrayness
 * analysis. In this test file we inject the stage directly to verify its behavior in various scenarios.
 *
 * @tags: [
 *   requires_fcv_90,
 *   # This stage is test-only and requires test commands
 *   requires_non_retryable_commands,
 * ]
 */

import {runWithKnobs} from "jstests/libs/property_test_helpers/common_properties.js";

const coll = db[jsTestName()];
coll.drop();

// Insert test documents
assert.commandWorked(
    coll.insert([
        {_id: 1, a: 1, b: "string", c: {d: 10}},
        {_id: 2, a: 2, b: "another", c: {d: 20}},
        {_id: 3, a: 3, b: "text", c: {d: 30, e: [1, 2]}}, // Has an array in c.e
    ]),
);

// Test 1: Empty paths set allows all documents through
{
    runWithKnobs(
        db,
        () => {
            const result = coll.aggregate([{$_internalAssertDataAssumptions: {paths: []}}]).toArray();

            assert.eq(result.length, 3, "Expected all documents to pass with empty path set");
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 2: Scalar fields pass validation
{
    runWithKnobs(
        db,
        () => {
            const result = coll
                .aggregate([{$match: {_id: {$lte: 2}}}, {$_internalAssertDataAssumptions: {paths: ["a", "b", "c.d"]}}])
                .toArray();

            assert.eq(result.length, 2, "Expected scalar field documents to pass validation");
            assert.eq(result[0]._id, 1);
            assert.eq(result[1]._id, 2);
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 3: Validation passes when fields don't exist
{
    runWithKnobs(
        db,
        () => {
            const result = coll
                .aggregate([
                    {$match: {_id: 1}},
                    {$_internalAssertDataAssumptions: {paths: ["nonExistent", "alsoMissing"]}},
                ])
                .toArray();

            assert.eq(result.length, 1, "Expected missing fields to pass validation");
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 4: Array field fails validation
{
    runWithKnobs(
        db,
        () => {
            try {
                // Insert a document with an array in field 'a'
                coll.insert({_id: 100, a: [1, 2, 3], b: "test"});

                // Verify the stage is correctly added to the pipeline via explain.
                const explain = coll
                    .explain()
                    .aggregate([{$match: {_id: 100}}, {$_internalAssertDataAssumptions: {paths: ["a"]}}]);
                assert(
                    tojson(explain).includes("$_internalAssertDataAssumptions"),
                    "Expected $_internalAssertDataAssumptions in explain output: " + tojson(explain),
                );

                const error = assert.throws(() => {
                    coll.aggregate([{$match: {_id: 100}}, {$_internalAssertDataAssumptions: {paths: ["a"]}}]).toArray();
                });

                assert.commandFailedWithCode(error, 12508302, "Expected validation to fail for array field");
                assert(
                    error.message.includes("canPathBeArray"),
                    "Error message should mention canPathBeArray: " + error.message,
                );
                assert(
                    error.message.includes("field 'a'"),
                    "Error message should mention the field name: " + error.message,
                );
            } finally {
                // Clean up
                coll.deleteOne({_id: 100});
            }
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 5: Nested array field fails validation
{
    runWithKnobs(
        db,
        () => {
            const error = assert.throws(() => {
                coll.aggregate([{$match: {_id: 3}}, {$_internalAssertDataAssumptions: {paths: ["c.e"]}}]).toArray();
            });

            assert.commandFailedWithCode(error, 12508302, "Expected validation to fail for nested array");
            assert(
                error.message.includes("c.e"),
                "Error message should mention the nested field path: " + error.message,
            );
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 6: Multiple fields validation
{
    runWithKnobs(
        db,
        () => {
            const result = coll
                .aggregate([{$match: {_id: {$lte: 2}}}, {$_internalAssertDataAssumptions: {paths: ["a", "b", "c.d"]}}])
                .toArray();

            assert.eq(result.length, 2, "Expected documents with all scalar fields to pass");
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 7: Stage works in middle of pipeline
{
    runWithKnobs(
        db,
        () => {
            const result = coll
                .aggregate([
                    {$match: {_id: {$lte: 2}}},
                    {$_internalAssertDataAssumptions: {paths: ["a"]}},
                    {$project: {a: 1, b: 1}},
                    {$_internalAssertDataAssumptions: {paths: ["b"]}},
                    {$sort: {_id: 1}},
                ])
                .toArray();

            assert.eq(result.length, 2, "Expected validation stages in middle of pipeline to work");
            assert.eq(result[0]._id, 1);
            assert.eq(result[1]._id, 2);
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 8: Validation stage is actually executed during normal aggregation
{
    runWithKnobs(
        db,
        () => {
            // Insert a document with an array
            coll.insert({_id: 200, a: [1, 2], b: "test"});

            try {
                // Without explain, the validation stage should execute and catch the array
                const error = assert.throws(() => {
                    coll.aggregate([{$match: {_id: 200}}, {$_internalAssertDataAssumptions: {paths: ["a"]}}]).toArray();
                });

                assert.commandFailedWithCode(error, 12508302, "Validation should fail during normal execution");

                // But with explain, it should succeed (stage not inserted)
                const explain = coll
                    .explain()
                    .aggregate([{$match: {_id: 200}}, {$_internalAssertDataAssumptions: {paths: ["a"]}}]);
                assert(explain, "Explain should succeed even though document has array");

                jsTest.log("Verified validation stage executes during normal run but not during explain");
            } finally {
                coll.deleteOne({_id: 200});
            }
        },
        {internalEnableDependencyGraphValidation: false},
    );
}

// Test 9: Validation stages with dependency graph knob should not appear in explain (with automatic injection)
{
    runWithKnobs(
        db,
        () => {
            // Run explain with the knob enabled - validation stages should be skipped
            const pipeline = [{$match: {_id: {$lte: 2}}}, {$project: {a: 1, b: 1}}];

            const explain = coll.explain("executionStats").aggregate(pipeline);

            // Search through all stages in the explain output
            const checkForValidationStage = (obj) => {
                if (typeof obj !== "object" || obj === null) {
                    return false;
                }
                if (obj.hasOwnProperty("$_internalAssertDataAssumptions")) {
                    return true;
                }
                for (let key in obj) {
                    if (checkForValidationStage(obj[key])) {
                        return true;
                    }
                }
                return false;
            };

            const hasValidationStage = checkForValidationStage(explain);
            assert(
                !hasValidationStage,
                "Validation stages should not appear in explain output even with knob enabled: " + tojson(explain),
            );

            jsTest.log("Verified auto-inserted validation stages don't appear in explain output");
        },
        {internalEnableDependencyGraphValidation: true},
    );
}

jsTest.log("$_internalAssertDataAssumptions tests completed successfully");
