/**
 * Tests that the server properly respects the maxBSONDepth parameter, and will fail to start up if
 * given an invalid depth.
 */
(function() {
    "use strict";

    const kTestName = "max_bson_depth_parameter";

    // Start mongod with a valid BSON depth, then test that it accepts and rejects command
    // appropriately based on the depth.
    let conn = MongoRunner.runMongod({setParameter: "maxBSONDepth=5"});
    assert.neq(null, conn, "Failed to start mongod");
    let testDB = conn.getDB("test");
    assert.commandWorked(testDB.runCommand({ping: 1}), "Failed to run a command on the server");
    assert.commandFailedWithCode(
        testDB.runCommand({find: "coll", filter: {x: {x: {x: {x: {x: {x: 1}}}}}}}),
        ErrorCodes.Overflow,
        "Expected server to reject command for exceeding the nesting depth limit");

    // Restart mongod with a negative maximum BSON depth and test that it fails to start.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({setParameter: "maxBSONDepth=-4"});
    assert.eq(null, conn, "Expected mongod to fail at startup because depth was negative");

    conn = MongoRunner.runMongod({setParameter: "maxBSONDepth=1"});
    assert.eq(null, conn, "Expected mongod to fail at startup because depth was too low");
}());
