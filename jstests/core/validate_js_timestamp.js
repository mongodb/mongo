/**
 * Test validation of Timestamp objects that get returned from $function execution, whether they are
 * returned directly or as a value in a document.
 *
 * The Timestamp constructor ensures that users cannot _create_ an invalid timestamp, but there is
 * nothing stopping a user function from modifying the timestamp afterwards with invalid values.
 *
 * @tags: [
 *   requires_fcv_63,
 * ]
 */
(function() {
"use strict";

/**
 * Each test case executes a pipeline (created by the `testPipeline' function below) that mutates a
 * Timestamp object according to the 'assignments' field and ensures that execution fails with the
 * expected error code and error message.
 */
const testCases = [
    {
        assignments: {t: 20000000000},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp time"
    },
    {
        assignments: {i: 20000000000},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp increment"
    },
    {
        assignments: {t: -1},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp time"
    },
    {
        assignments: {i: -1},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp increment"
    },
    {
        assignments: {t: "str"},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp time"
    },
    {
        assignments: {i: "str"},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp increment"
    },
    {
        assignments: {t: {foo: "bar"}},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp time"
    },
    {
        assignments: {i: {foo: "bar"}},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp increment"
    },
    {
        assignments: {t: [2]},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp time"
    },
    {
        assignments: {i: [2]},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp increment"
    },
    {
        assignments: {t: "str1", i: "str2"},
        expectedErrorCode: ErrorCodes.BadValue,
        errorShouldContain: "Timestamp"
    },
    {
        assignments: {t: "REMOVE"},
        expectedErrorCode: 6900900,
        errorShouldContain: "missing timestamp field"
    },
    {
        assignments: {i: "REMOVE"},
        expectedErrorCode: 6900901,
        errorShouldContain: "missing increment field"
    },
    {
        assignments: {i: "REMOVE", t: "REMOVE"},
        expectedErrorCode: [6900900, 6900901],
        errorShouldContain: "missing"
    },
];

/**
 * The test pipeline evaluates a $function expression on exactly one document, passing 'assignments'
 * and 'embedInObject' as its arguments.
 *
 * The test function creates a valid Timestamp but then mutates it, bypassing any validation checks,
 * and returns it, either as a scalar or as a value in a document, depending on the 'embedInObject'
 * argument.
 *
 * Mutation follows the specification in the 'assignments' argument: each field-value pair (f, v) in
 * 'assignments' is executed as a 'timestamp.f = v' assignemnt, except when v is the special
 * "REMOVE" value, which deletes the field from the timestamp.
 */
function testPipeline(assignments, embedInObject) {
    return [
        {$documents: [{unvalidatedUserData: assignments, embedInObject: embedInObject}]},
        {
            $project: {
                computedField: {
                    $function: {
                        body: function(assignments, embedInObject) {
                            let timestamp = Timestamp(1, 1);

                            for (let [field, value] of Object.entries(JSON.parse(assignments))) {
                                if (value !== "REMOVE") {
                                    timestamp[field] = value;
                                } else {
                                    delete timestamp[field];
                                }
                            }

                            return embedInObject ? {result: timestamp} : timestamp;
                        },
                        args: ["$unvalidatedUserData", "$embedInObject"],
                        lang: "js"
                    }
                }
            }
        }
    ];
}

/**
 * Execute each test case twice: once with 'embedInObject' set to false and once with it set to
 * true.
 */
for (let {assignments, expectedErrorCode, errorShouldContain} of testCases) {
    let error = assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: 1, pipeline: testPipeline(tojson(assignments), false), cursor: {}}),
        expectedErrorCode);
    assert(error.errmsg.indexOf(errorShouldContain) >= 0, error);

    error = assert.commandFailedWithCode(
        db.runCommand(
            {aggregate: 1, pipeline: testPipeline(tojson(assignments), true), cursor: {}}),
        expectedErrorCode);
    assert(error.errmsg.indexOf(errorShouldContain) >= 0, error);
}

/**
 * Additionally, test a case where the function execution makes a legal modification to a
 * Timestamp object, producing a valid timestamp.
 */
let result = db.aggregate(testPipeline(tojson({t: 123.0, i: 456.0}), false)).toArray();
assert.sameMembers(result, [{computedField: Timestamp(123, 456)}]);

result = db.aggregate(testPipeline(tojson({t: 123.0, i: 456.0}), true)).toArray();
assert.sameMembers(result, [{computedField: {result: Timestamp(123, 456)}}]);
}());
