// Test various error cases of $regexMatch, $regexFind and $regexFindAll aggregation expressions.
(function() {
'use strict';

load('jstests/libs/sbe_assert_error_override.js');  // Override error-code-checking APIs.
load('jstests/aggregation/extras/utils.js');        // For assertErrorCode().

const coll = db.regex_error_cases;
coll.drop();

function assertFails(parameters, errorCode) {
    // Check constant parameters.
    let inputDocument = {text: 'ABCD'};
    assert.commandWorked(coll.insert(inputDocument));

    const constantParameters = Object.assign({input: '$text'}, parameters);
    assertErrorCode(coll, [{$project: {result: {'$regexMatch': constantParameters}}}], errorCode);
    assertErrorCode(coll, [{$project: {result: {'$regexFind': constantParameters}}}], errorCode);
    assertErrorCode(coll, [{$project: {result: {'$regexFindAll': constantParameters}}}], errorCode);

    // Check constant parameters, but without optimization phase.
    try {
        assert.commandWorked(db.adminCommand(
            {'configureFailPoint': 'disablePipelineOptimization', 'mode': 'alwaysOn'}));

        assertErrorCode(
            coll, [{$project: {result: {'$regexMatch': constantParameters}}}], errorCode);
        assertErrorCode(
            coll, [{$project: {result: {'$regexFind': constantParameters}}}], errorCode);
        assertErrorCode(
            coll, [{$project: {result: {'$regexFindAll': constantParameters}}}], errorCode);
    } finally {
        assert.commandWorked(
            db.adminCommand({'configureFailPoint': 'disablePipelineOptimization', 'mode': 'off'}));
    }

    assert(coll.drop());

    // Check parameters pulled from collection.
    inputDocument = Object.assign(inputDocument, parameters);
    assert.commandWorked(coll.insert(inputDocument));

    const dynamicParameters = {input: '$text'};
    if ('regex' in parameters) {
        dynamicParameters.regex = '$regex';
    }
    if ('options' in parameters) {
        dynamicParameters.options = '$options';
    }
    assertErrorCode(coll, [{$project: {result: {'$regexMatch': dynamicParameters}}}], errorCode);
    assertErrorCode(coll, [{$project: {result: {'$regexFind': dynamicParameters}}}], errorCode);
    assertErrorCode(coll, [{$project: {result: {'$regexFindAll': dynamicParameters}}}], errorCode);
    assert(coll.drop());
}

// Regex pattern must be string, BSON RegEx or null.
assertFails({regex: 123}, 51105);

// Regex flags must be string or null.
assertFails({regex: '.*', options: 123}, 51106);

// Options cannot be specified both in BSON RegEx and in options field.
assertFails({regex: /.*/i, options: 's'}, 51107);

// Regex pattern cannot contain null bytes.
assertFails({regex: '[a-b]+\0[c-d]+'}, 51109);

// Regex flags cannot contain null bytes (even if regex pattern is null).
assertFails({regex: '.*', options: 'i\0s'}, 51110);
assertFails({regex: null, options: 'i\0s'}, 51110);

// Regex pattern must be a valid regular expression.
assertFails({regex: '[a-'}, 51111);

// Regex flags must be valid.
assertFails({regex: '.*', options: 'ish'}, 51108);
}());