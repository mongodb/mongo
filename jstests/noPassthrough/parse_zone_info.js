// Tests the parsing of the timeZoneInfo parameter.
(function() {
// Test that a bad file causes startup to fail.
let conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/bad_timezone_info"});
assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");
assert.neq(-1, rawMongoProgramOutput().search(/Fatal assertion.*40475/));

// Test that a non-existent directory causes startup to fail.
conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/missing_directory"});
assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");

// Look for either old or new error message
assert(rawMongoProgramOutput().includes("Error creating service context") ||
       rawMongoProgramOutput().includes("Failed to create service context"));

// Test that startup can succeed with a good file.
conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/good_timezone_info"});
assert.neq(conn, null, "expected launching mongod with good timezone rules to succeed");
MongoRunner.stopMongod(conn);
}());
