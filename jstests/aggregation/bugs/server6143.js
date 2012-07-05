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

// Clear db
db.s6143.drop();

// Populate db
db.s6143.save({a:null});

// Aggregate using a date expression on a null value
var s6143 = db.runCommand(
{ aggregate: "s6143", pipeline : [
    { $project : {
        dateConvert : { $dayOfWeek: ["$a"] }
    }}
]});

// Result should be the following error document
s6143result = {
    "result" : [],
    "errmsg" : "exception: can't convert from BSON type NULL to Date",
    "code" : 16006,
    "ok" : 0
}

// Assert
assert.eq(s6143, s6143result, 's6143 failed');
