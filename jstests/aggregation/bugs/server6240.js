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

// Function for checking
function check_answer(agg_result, errno) {
    assert.eq(agg_result.ok, 0, 's6240 failed');
    assert.eq(agg_result.code, errno, 's6240 failed');
}

// Clear db
db.s6240.drop();

// Populate db
db.s6240.save({date:new Date()});

// Aggregate using a date value in various math operations
// Add
var s6240add = db.runCommand(
    { aggregate: "s6240", pipeline: [
        { $project: {
            add: { $add: ["$date", "$date"] }
    }}
]});
check_answer(s6240add, 16612);


// Divide
var s6240divide = db.runCommand(
    { aggregate: "s6240", pipeline: [
        { $project: {
            divide: { $divide: ["$date", 2] }
    }}
]});
check_answer(s6240divide, 16373);

// Mod
var s6240mod = db.runCommand(
    { aggregate: "s6240", pipeline: [
        { $project: {
            mod: { $mod: ["$date", 2] }
    }}
]});
check_answer(s6240mod, 16374);


// Multiply
var s6240multiply = db.runCommand(
    { aggregate: "s6240", pipeline: [
        { $project: {
            multiply: { $multiply: ["$date", 2] }
    }}
]});
check_answer(s6240multiply, 16375);


// Subtract
var s6240subtract = db.runCommand(
    { aggregate: "s6240", pipeline: [
        { $project: {
            subtract: { $subtract: [2, "$date"] }
    }}
]});
check_answer(s6240subtract, 16614);
