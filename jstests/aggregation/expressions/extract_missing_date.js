/**
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

import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
// Clear db
coll.drop();

// Populate db
assert.commandWorked(coll.insertOne({date: new Date()}));

// Aggregate using a date value in various math operations
// Add
assertErrorCode(coll, {$project: {add: {$add: ["$date", "$date"]}}}, 16612);

// Divide
assertErrorCode(
    coll, {$project: {divide: {$divide: ["$date", 2]}}}, [16609, ErrorCodes.TypeMismatch]);

// Mod
assertErrorCode(coll, {$project: {mod: {$mod: ["$date", 2]}}}, 16611);

// Multiply
assertErrorCode(
    coll, {$project: {multiply: {$multiply: ["$date", 2]}}}, [16555, ErrorCodes.TypeMismatch]);

// Subtract
assertErrorCode(
    coll, {$project: {subtract: {$subtract: [2, "$date"]}}}, [16556, ErrorCodes.TypeMismatch]);
