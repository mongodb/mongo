// Tests the parsing of the timeZoneInfo parameter and file use.
(function() {
// Test that a bad file causes startup to fail.
let conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/bad_timezone_info"});
assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");
assert.neq(-1, rawMongoProgramOutput().indexOf("Fatal assertion 40475"));

// Test that a non-existent directory causes startup to fail.
conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/missing_directory"});
assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");

// Look for either old or new error message
assert(rawMongoProgramOutput().indexOf("Failed to create service context") != -1 ||
       rawMongoProgramOutput().indexOf("Failed global initialization") != -1);

// Test that startup can succeed with a good file.
conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/good_timezone_info"});
assert.neq(conn, null, "expected launching mongod with good timezone rules to succeed");

// Test that can use file-provided timezones in an expression.
const testDB = conn.getDB("test");
const coll = testDB.parse_zone_info;
assert.commandWorked(coll.insert({x: new Date()}));
assert.doesNotThrow(
    () => coll.aggregate([{$set: {x_parts: {$dateToParts: {date: "$x", timezone: "GMT"}}}}]));
assert.doesNotThrow(
    () => coll.aggregate(
        [{$set: {x_parts: {$dateToParts: {date: "$x", timezone: "America/Sao_Paulo"}}}}]));

// Confirm that attempt to use a non-existent timezone in an expression fails.
const err = assert.throws(
    () => coll.aggregate([{$set: {x_parts: {$dateToParts: {date: "$x", timezone: "Unknown"}}}}]));
assert.eq(err.code, 40485);
MongoRunner.stopMongod(conn);
}());
