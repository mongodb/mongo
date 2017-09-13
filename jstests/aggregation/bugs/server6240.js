/*
 * SERVER-6240: verify assertion on attempt to perform date extraction from missing or null value
 *
 * This test validates the SERVER-6240 ticket. uassert when attempting to extract a date from a
 * null value. Prevously verify'd.
 *
 * This test also validates the error cases for SERVER-6239 (support $add and $subtract with dates)
 */

/*
 * 1) Clear then populate testing db
 * 2) Run an aggregation that uses a date value in various math operations
 * 3) Assert that we get the correct error
 */

// Load test utilities
load('jstests/aggregation/extras/utils.js');

// Clear db
db.s6240.drop();

// Populate db
db.s6240.save({date: new Date()});

// Aggregate using a date value in various math operations
// Add
assertErrorCode(db.s6240, {$project: {add: {$add: ["$date", "$date"]}}}, 16612);

// Divide
assertErrorCode(db.s6240, {$project: {divide: {$divide: ["$date", 2]}}}, 16609);

// Mod
assertErrorCode(db.s6240, {$project: {mod: {$mod: ["$date", 2]}}}, 16611);

// Multiply
assertErrorCode(db.s6240, {$project: {multiply: {$multiply: ["$date", 2]}}}, 16555);

// Subtract
assertErrorCode(db.s6240, {$project: {subtract: {$subtract: [2, "$date"]}}}, 16556);
