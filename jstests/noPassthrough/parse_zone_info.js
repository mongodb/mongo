// Tests the parsing of the timeZoneInfo parameter.
(function() {
    // Test that a bad file causes startup to fail.
    let conn = MerizoRunner.runMerizod({timeZoneInfo: "jstests/libs/config_files/bad_timezone_info"});
    assert.eq(conn, null, "expected launching merizod with bad timezone rules to fail");
    assert.neq(-1, rawMerizoProgramOutput().indexOf("Fatal assertion 40475"));

    // Test that a non-existent directory causes startup to fail.
    conn = MerizoRunner.runMerizod({timeZoneInfo: "jstests/libs/config_files/missing_directory"});
    assert.eq(conn, null, "expected launching merizod with bad timezone rules to fail");

    // Look for either old or new error message
    assert(rawMerizoProgramOutput().indexOf("Failed to create service context") != -1 ||
           rawMerizoProgramOutput().indexOf("Failed global initialization") != -1);

    // Test that startup can succeed with a good file.
    conn = MerizoRunner.runMerizod({timeZoneInfo: "jstests/libs/config_files/good_timezone_info"});
    assert.neq(conn, null, "expected launching merizod with good timezone rules to succeed");
    MerizoRunner.stopMerizod(conn);
}());
