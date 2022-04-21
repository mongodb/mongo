/**
 * Utility for testing the parsing of densify-like commands. I.e. $_internalDensify and $densify.
 */

let parseUtil = (function(db, coll, stageName, options = {}) {
    function run(stage, extraCommandArgs = options) {
        return coll.runCommand(Object.merge(
            {aggregate: coll.getName(), pipeline: [stage], cursor: {}}, extraCommandArgs));
    }

    function runTest(stageName) {
        // Required fields.
        const kIDLRequiredFieldErrorCode = 40414;
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: 1.0}}}),
            kIDLRequiredFieldErrorCode,
            "BSON field '$densify.range.bounds' is missing but a required field");
        assert.commandFailedWithCode(run({[stageName]: {range: {step: 1.0, bounds: "full"}}}),
                                     kIDLRequiredFieldErrorCode,
                                     "BSON field '$densify.field' is missing but a required field");
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {bounds: "full"}}}),
            kIDLRequiredFieldErrorCode,
            "BSON field '$densify.range.step' is missing but a required field");

        // Wrong types
        assert.commandFailedWithCode(
            run({[stageName]: {field: 1.0, range: {step: 1.0, bounds: "full"}}}),
            ErrorCodes.TypeMismatch,
            "BSON field '$densify.field' is the wrong type 'double', expected type 'string'");
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: "invalid", bounds: "full"}}}),
            ErrorCodes.TypeMismatch,
            "BSON field '$densify.range.step' is the wrong type 'string', expected types '[int, decimal, double, long']");
        assert.commandFailedWithCode(
            run({
                [stageName]:
                    {field: "a", partitionByFields: "incorrect", range: {step: 1.0, bounds: "full"}}
            }),
            ErrorCodes.TypeMismatch,
            "BSON field '$densify.partitionByFields' is the wrong type 'string', expected type 'array'");
        assert.commandFailedWithCode(
            run({
                [stageName]: {
                    field: "a",
                    range: {
                        step: 1.0,
                        bounds: [new ISODate("2020-01-01"), new ISODate("2020-01-02")],
                        unit: 1000
                    }
                }
            }),
            ErrorCodes.TypeMismatch,
            "BSON field '$densify.range.unit' is the wrong type 'double', expected type 'string'");

        // Logical errors
        // Too few bounds
        assert.commandFailedWithCode(
            run({
                [stageName]: {
                    field: "a",
                    range: {step: 1.0, bounds: [new ISODate("2020-01-01")], unit: "second"}
                }
            }),
            5733403,
            "a bounding array in a range statement must have exactly two elements");
        // Too many elements
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: 1.0, bounds: [0, 1, 2]}}}),
            5733403,
            "a bounding array in a range statement must have exactly two elements");
        // Negative step
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: -1.0, bounds: [0, 1]}}}),
            5733401,
            "the step parameter in a range statement must be a strictly positive numeric value");
        // Field path expression instead of field path
        assert.commandFailedWithCode(
            run({[stageName]: {field: "$a", range: {step: 1.0, bounds: [0, 1]}}}),
            16410,
            "FieldPath field names may not start with '$'. Consider using $getField or $setField.");
        // Field path expression instead of field path
        assert.commandFailedWithCode(
            run({
                [stageName]:
                    {field: "a", partitionByFields: ["$b"], range: {step: 1.0, bounds: [0, 1]}}
            }),
            16410,
            "FieldPath field names may not start with '$'. Consider using $getField or $setField.");
        // Partition bounds but not partitionByFields
        assert.commandFailedWithCode(
            run({
                [stageName]:
                    {field: "a", partitionByFields: [], range: {step: 1.0, bounds: "partition"}}
            }),
            5733408,
            "one may not specify the bounds as 'partition' without specifying a non-empty array of partitionByFields. You may have meant to specify 'full' bounds.");
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: 1.0, bounds: "partition"}}}),
            5733408,
            "one may not specify the bounds as 'partition' without specifying a non-empty array of partitionByFields. You may have meant to specify 'full' bounds.");
        // Left bound greater than right bound
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: 1.0, bounds: [2, 1]}}}),
            5733402,
            "the bounds in a range statement must be the string 'full', 'partition', or an ascending array of two numbers or two dates");
        assert.commandFailedWithCode(
            run({
                [stageName]: {
                    field: "a",
                    range: {
                        step: 1.0,
                        bounds: [new ISODate("2020-01-01"), new ISODate("2019-01-01")],
                        unit: "second"
                    }
                }
            }),
            5733402,
            "the bounds in a range statement must be the string 'full', 'partition', or an ascending array of two numbers or two dates");
        // Unit with numeric bounds
        assert.commandFailedWithCode(
            run({[stageName]: {field: "a", range: {step: 1.0, bounds: [1, 2], unit: "second"}}}),
            5733409,
            "numeric bounds may not have unit parameter");
        // Mixed numeric and date bounds
        assert.commandFailedWithCode(
            run({
                [stageName]:
                    {field: "a", range: {step: 1.0, bounds: [1, new ISODate("2020-01-01")]}}
            }),
            5733406,
            "a bounding array must contain either both dates or both numeric types");
        assert.commandFailedWithCode(
            run({
                [stageName]: {
                    field: "a",
                    range: {step: 1.0, bounds: [new ISODate("2020-01-01"), 1], unit: "second"}
                }
            }),
            5733402,
            "a bounding array must be an ascending array of either two dates or two numbers");
        // Non-whole number step with date bounds
        assert.commandFailedWithCode(
            run({
                [stageName]: {
                    field: "a",
                    range: {
                        step: 1.1,
                        bounds: [new ISODate("2020-01-01"), new ISODate("2020-01-03")],
                        unit: "second"
                    }
                }
            }),
            6586400,
            "The step parameter in a range satement must be a whole number when densifying a date range");

        // Positive test cases
        assert.commandWorked(run({[stageName]: {field: "a", range: {step: 1.0, bounds: [1, 2]}}}));
        assert.commandWorked(run({
            [stageName]: {
                field: "a",
                range: {
                    step: 1.0,
                    bounds: [new ISODate("2020-01-01"), new ISODate("2021-01-01")],
                    unit: "day"
                }
            }
        }));
        assert.commandWorked(run({
            [stageName]: {
                field: "a",
                partitionByFields: ["b", "c"],
                range: {
                    step: 1.0,
                    bounds: [new ISODate("2020-01-01"), new ISODate("2021-01-01")],
                    unit: "week"
                }
            }
        }));
        assert.commandWorked(run({
            [stageName]: {
                field: "a",
                partitionByFields: ["b", "c"],
                range: {step: 1.0, bounds: "partition", unit: "second"}
            }
        }));
        assert.commandWorked(run({
            [stageName]: {
                field: "a",
                partitionByFields: ["b", "c"],
                range: {step: 1.0, bounds: "full", unit: "second"}
            }
        }));
        assert.commandWorked(run({
            [stageName]:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "full"}}
        }));
        assert.commandWorked(run({
            [stageName]: {
                field: "a",
                partitionByFields: [
                    "b",
                ],
                range: {step: 1.0, bounds: "partition"}
            }
        }));
        assert.commandWorked(run({[stageName]: {field: "a", range: {step: 1.0, bounds: "full"}}}));
    }

    runTest(stageName);
});
