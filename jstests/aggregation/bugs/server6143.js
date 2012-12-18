/*
 * SERVER-6143 : verify assertion on attempt to perform date extraction from missing or null value
 *
 * This test validates the SERVER-6143 ticket. uassert when attempting to extract a date from a
 * null value. Prevously verify'd.
 */

/*
 * 1) Clear then populate testing db
 * 2) Run an aggregation that uses a date command on a null value
 * 3) Assert that we get the correct error
 */

// load the test utilities
load('jstests/aggregation/extras/utils.js');

// Clear db
db.s6143.drop();

// Populate db
db.s6143.save({a:null});

// Aggregate using a date expression on a null value, assert error
assertErrorCode(db.s6143,
                { $project : {dateConvert : {$dayOfWeek:["$a"]}}},
                16006);

