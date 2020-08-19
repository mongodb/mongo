/**
 * Tests update and findAndModify command behavior with update modifiers.
 *
 * @tags: [
 * requires_fcv_47,
 * # The test is designed to work with an unsharded collection.
 * assumes_unsharded_collection,
 * # The coll.update command does not work with $set operator in compatibility write mode.
 * requires_find_command,
 * # Performs modifications that if repeated would fail the test.
 * requires_non_retryable_writes,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.update_modifiers;

// Executes a test case which inserts documents into the test collection, issues an update command,
// and verifies that the results are as expected.
function executeUpdateTestCase(testCase) {
    jsTestLog(tojson(testCase));

    // Remove all existing documents and then insert the new test case's documents.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(testCase.inputDocuments));

    // Issue the update command specified in the test case.
    const result = testCase.command(coll, testCase.query, testCase.update, testCase.arrayFilters);

    if (testCase.expectedErrorCode == undefined) {
        // Verify that the command succeeded and collection's contents match the expected results.
        assert.commandWorked(result);
        assert.docEq(coll.find({}).sort({_id: 1}).toArray(), testCase.expectedResults);
    } else {
        assert.commandFailedWithCode(result, testCase.expectedErrorCode);
    }
}

// Issues the update command and returns the response.
function updateCommand(coll, query, update, arrayFilters) {
    const commandOptions = {upsert: true};
    if (arrayFilters !== undefined) {
        commandOptions.arrayFilters = arrayFilters;
    }
    return coll.update(query, update, commandOptions);
}

// Issues the findAndModify command and returns the response.
function findAndModifyCommand(coll, query, update, arrayFilters) {
    const command = {query: query, update: update, upsert: true};
    if (arrayFilters !== undefined) {
        command.arrayFilters = arrayFilters;
    }
    return coll.runCommand("findAndModify", command);
}

// Tests all relevant update modifiers with set and empty update documents.
const testCases = [
    {
        query: {_id: 1},
        update: {$set: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 2},
        update: {$set: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}, {_id: 2}],
    },
    {
        query: {_id: 1},
        update: {$set: {a: 2}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 2}],
    },
    {
        query: {_id: 1},
        update: {$set: {}},
        arrayFilters: [{"element": {$gt: 6}}],
        inputDocuments: [{_id: 1, a: [1]}],
        expectedErrorCode: ErrorCodes.FailedToParse,
    },
    {
        query: {_id: 1},
        update: {$unset: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$unset: {a: 1}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1}],
    },
    {
        query: {_id: 1},
        update: {$inc: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$inc: {a: 1}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 2}],
    },
    {
        query: {_id: 1},
        update: {$mul: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$mul: {a: 2}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 2}],
    },
    {
        query: {_id: 1},
        update: {$push: {}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: [1]}],
    },
    {
        query: {_id: 1},
        update: {$push: {a: 2}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: [1, 2]}],
    },
    {
        query: {_id: 1},
        update: {$addToSet: {}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: [1]}],
    },
    {
        query: {_id: 1},
        update: {$addToSet: {a: 2}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: [1, 2]}],
    },
    {
        query: {_id: 1},
        update: {$pull: {}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: [1]}],
    },
    {
        query: {_id: 1},
        update: {$pull: {a: 1}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: []}],
    },
    {
        query: {_id: 1},
        update: {$rename: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$rename: {a: "b"}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, b: 1}],
    },
    {
        query: {_id: 1},
        update: {$bit: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$bit: {a: {and: NumberLong(1)}}},
        inputDocuments: [{_id: 1, a: NumberLong(2)}],
        expectedResults: [{_id: 1, a: NumberLong(0)}],
    },
    {
        query: {_id: 1},
        update: {$bit: {a: {and: NumberLong(3)}}},
        inputDocuments: [],
        expectedResults: [{_id: 1, a: NumberLong(0)}],
    },
    {
        query: {_id: 1},
        update: {$bit: {b: {or: NumberLong(3)}}},
        inputDocuments: [],
        expectedResults: [{_id: 1, b: NumberLong(3)}],
    },
    {
        query: {_id: 1},
        update: {$bit: {"c.d": {or: NumberInt(3)}}},
        inputDocuments: [],
        expectedResults: [{_id: 1, c: {d: NumberInt(3)}}],
    },
    {
        query: {_id: 1},
        update: {$max: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$max: {a: 2}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 2}],
    },
    {
        query: {_id: 1},
        update: {$min: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$min: {a: 0}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 0}],
    },
    {
        query: {_id: 1},
        update: {$currentDate: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$setOnInsert: {}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 2},
        update: {$setOnInsert: {a: 1}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}, {_id: 2, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$setOnInsert: {a: 2}},
        inputDocuments: [{_id: 1, a: 1}],
        expectedResults: [{_id: 1, a: 1}],
    },
    {
        query: {_id: 1},
        update: {$pop: {}},
        inputDocuments: [{_id: 1, a: [1]}],
        expectedResults: [{_id: 1, a: [1]}],
    },
    {
        query: {_id: 1},
        update: {$pop: {a: true}},
        inputDocuments: [{_id: 1, a: [1, 2]}],
        expectedErrorCode: ErrorCodes.FailedToParse,
    },
    {
        query: {_id: 1},
        update: {$pop: {a: 1}},
        inputDocuments: [{_id: 1, a: [1, 2]}],
        expectedResults: [{_id: 1, a: [1]}],
    },
    {
        query: {_id: 1},
        update: {$pop: {a: -1}},
        inputDocuments: [{_id: 1, a: [1, 2]}],
        expectedResults: [{_id: 1, a: [2]}],
    },
    {
        query: {_id: 1},
        update: {$pop: {a: 1}},
        inputDocuments: [{_id: 1, a: []}],
        expectedResults: [{_id: 1, a: []}],
    },
    {
        query: {_id: 1},
        update: {$pop: {a: 1}},
        inputDocuments: [],
        expectedResults: [{_id: 1}],
    },
    {
        query: {_id: 1},
        update: {$currentDate: {}},
        inputDocuments: [{_id: 1}],
        expectedResults: [{_id: 1}],
    },
    {
        query: {_id: 1},
        update: {$pullAll: {}},
        inputDocuments: [{_id: 1, a: [1, 2]}],
        expectedResults: [{_id: 1, a: [1, 2]}],
    },
    {
        query: {_id: 1},
        update: {$pullAll: {a: [1]}},
        inputDocuments: [{_id: 1, a: [1, 2, 1]}],
        expectedResults: [{_id: 1, a: [2]}],
    },
];

for (const command of [updateCommand, findAndModifyCommand]) {
    for (const testCase of testCases) {
        executeUpdateTestCase(Object.assign({command: command}, testCase));
    }
}
})();
