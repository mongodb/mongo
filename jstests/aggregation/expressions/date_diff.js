/**
 * Tests $dateDiff expression.
 * @tags: [
 *   sbe_incompatible,
 *   requires_fcv_49
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.collection;

// Drop the test database.
assert.commandWorked(testDB.dropDatabase());

// Executes a test case that inserts documents, issues an aggregate command on a collection and
// compares the results with the expected.
function executeTestCase(testCase) {
    jsTestLog(tojson(testCase));
    coll.remove({});

    // Insert some documents into the collection.
    assert.commandWorked(coll.insert(testCase.inputDocuments));

    // Issue an aggregate command and verify the result.
    try {
        const actualResults = coll.aggregate(testCase.pipeline).toArray();
        assert(testCase.expectedErrorCode === undefined,
               `Expected an exception with code ${testCase.expectedErrorCode}`);
        assert.eq(actualResults, testCase.expectedResults);
    } catch (error) {
        if (testCase.expectedErrorCode === undefined) {
            throw error;
        }
        assert.eq(testCase.expectedErrorCode, error.code, tojson(error));
    }
}
const someDate = new Date("2020-11-01T18:23:36Z");
const testCases = [
    {
        // Parameters are constants, timezone is not specified.
        pipeline: [{
            $project: {
                _id: true,
                date_diff: {
                    $dateDiff: {
                        startDate: new Date("2020-11-01T18:23:36Z"),
                        endDate: new Date("2020-11-02T00:00:00Z"),
                        unit: "hour"
                    }
                }
            }
        }],
        inputDocuments: [{_id: 1}],
        expectedResults: [{_id: 1, date_diff: NumberLong("6")}]
    },
    {
        // Parameters are field paths.
        pipeline: [{
            $project: {
                _id: true,
                date_diff: {
                    $dateDiff: {
                        startDate: "$startDate",
                        endDate: "$endDate",
                        unit: "$units",
                        timezone: "$timeZone"
                    }
                }
            }
        }],
        inputDocuments: [{
            _id: 1,
            startDate: new Date("2020-11-01T18:23:36Z"),
            endDate: new Date("2020-11-02T00:00:00Z"),
            units: "hour",
            timeZone: "America/New_York"
        }],
        expectedResults: [{_id: 1, date_diff: NumberLong("6")}]
    },
    {
        // Invalid inputs.
        pipeline: [{
            $project: {
                _id: true,
                date_diff: {
                    $dateDiff: {
                        startDate: "$startDate",
                        endDate: "$endDate",
                        unit: "$units",
                        timezone: "$timeZone"
                    }
                }
            }
        }],
        inputDocuments:
            [{_id: 1, startDate: "string", endDate: someDate, units: "decade", timeZone: "UTC"}],
        expectedErrorCode: 5166307,
    }
];
testCases.forEach(executeTestCase);
}());