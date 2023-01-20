// Test various error cases of $regexMatch, $regexFind and $regexFindAll aggregation expressions.
(function() {
'use strict';

load('jstests/libs/sbe_assert_error_override.js');   // Override error-code-checking APIs.
load('jstests/libs/aggregation_pipeline_utils.js');  // For executeAggregationTestCase().

const coll = db.regex_error_cases;
coll.drop();

function assertFails(parameters, errorCode, allowNullResponse = false) {
    // Check constant parameters.
    let inputDocument = {text: 'ABCD'};

    const constantParameters = Object.assign({input: '$text'}, parameters);
    const regexMatchTest =
        Object.assign({inputDocuments: inputDocument, expectedErrorCode: errorCode},
                      allowNullResponse ? {expectedResults: [{"result": false}]} : {});
    const regexFindTest =
        Object.assign({inputDocuments: inputDocument, expectedErrorCode: errorCode},
                      allowNullResponse ? {expectedResults: [{"result": null}]} : {});
    const regexFindAllTest =
        Object.assign({inputDocuments: inputDocument, expectedErrorCode: errorCode},
                      allowNullResponse ? {expectedResults: [{"result": []}]} : {});

    executeAggregationTestCase(
        coll,
        Object.assign(
            {pipeline: [{$project: {"_id": 0, result: {'$regexMatch': constantParameters}}}]},
            regexMatchTest));
    executeAggregationTestCase(
        coll,
        Object.assign(
            {pipeline: [{$project: {"_id": 0, result: {'$regexFind': constantParameters}}}]},
            regexFindTest));
    executeAggregationTestCase(
        coll,
        Object.assign(
            {pipeline: [{$project: {"_id": 0, result: {'$regexFindAll': constantParameters}}}]},
            regexFindAllTest));

    // Check constant parameters, but without optimization phase.
    try {
        assert.commandWorked(db.adminCommand(
            {'configureFailPoint': 'disablePipelineOptimization', 'mode': 'alwaysOn'}));

        executeAggregationTestCase(
            coll,
            Object.assign(
                {pipeline: [{$project: {"_id": 0, result: {'$regexMatch': constantParameters}}}]},
                regexMatchTest));
        executeAggregationTestCase(
            coll,
            Object.assign(
                {pipeline: [{$project: {"_id": 0, result: {'$regexFind': constantParameters}}}]},
                regexFindTest));
        executeAggregationTestCase(
            coll,
            Object.assign(
                {pipeline: [{$project: {"_id": 0, result: {'$regexFindAll': constantParameters}}}]},
                regexFindAllTest));
    } finally {
        assert.commandWorked(
            db.adminCommand({'configureFailPoint': 'disablePipelineOptimization', 'mode': 'off'}));
    }

    // Check parameters pulled from collection.
    inputDocument = Object.assign(inputDocument, parameters);

    const dynamicParameters = {input: '$text'};
    if ('regex' in parameters) {
        dynamicParameters.regex = '$regex';
    }
    if ('options' in parameters) {
        dynamicParameters.options = '$options';
    }
    executeAggregationTestCase(
        coll,
        Object.assign(
            {pipeline: [{$project: {"_id": 0, result: {'$regexMatch': dynamicParameters}}}]},
            regexMatchTest));
    executeAggregationTestCase(
        coll,
        Object.assign(
            {pipeline: [{$project: {"_id": 0, result: {'$regexFind': dynamicParameters}}}]},
            regexFindTest));
    executeAggregationTestCase(
        coll,
        Object.assign(
            {pipeline: [{$project: {"_id": 0, result: {'$regexFindAll': dynamicParameters}}}]},
            regexFindAllTest));
}

// Regex pattern must be string, BSON RegEx or null.
assertFails({regex: 123}, 51105);

// Regex flags must be string or null.
assertFails({regex: '.*', options: 123}, 51106);

// Options cannot be specified both in BSON RegEx and in options field.
assertFails({regex: /.*/i, options: 's'}, 51107);

// Regex pattern cannot contain null bytes.
assertFails({regex: '[a-b]+\0[c-d]+'}, 51109);

// Regex flags cannot contain null bytes.
assertFails({regex: '.*', options: 'i\0s'}, 51110);
// If regex pattern is null, the query could either return null or report the 'cannot contain null
// bytes' error.
assertFails({regex: null, options: 'i\0s'}, 51110, true /* allowNullResponse */);

// Regex pattern must be a valid regular expression.
assertFails({regex: '[a-'}, 51111);

// Regex flags must be valid.
assertFails({regex: '.*', options: 'ish'}, 51108);
}());
