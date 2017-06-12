// Tests the parsing of the timeZoneInfo parameter.
(function() {
    // Test that a bad file causes startup to fail.
    let conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/bad_timezone_info"});
    assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");
    assert.neq(-1, rawMongoProgramOutput().indexOf("Fatal assertion 40475"));

    // Test that a non-existent directory causes startup to fail.
    conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/missing_directory"});
    assert.eq(conn, null, "expected launching mongod with bad timezone rules to fail");
    assert.neq(-1, rawMongoProgramOutput().indexOf("Failed global initialization"));

    // Test that startup can succeed with a good file.
    conn = MongoRunner.runMongod({timeZoneInfo: "jstests/libs/config_files/good_timezone_info"});
    assert.neq(conn, null, "expected launching mongod with good timezone rules to succeed");
    MongoRunner.stopMongod(conn);
}());
