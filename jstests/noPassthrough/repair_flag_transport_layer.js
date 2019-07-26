/*
 * SERVER-28990: Tests that a mongod started with --repair doesn't attempt binding to a port.
 */

(function() {
"use strict";
let dbpath = MongoRunner.dataPath + "repair_flag_transport_layer";
resetDbpath(dbpath);

function runTest(conn) {
    let returnCode =
        runNonMongoProgram("mongod", "--port", conn.port, "--repair", "--dbpath", dbpath);
    assert.eq(returnCode, 0, "expected mongod --repair to execute successfully regardless of port");
}

let conn = MongoRunner.runMongod();

runTest(conn);
MongoRunner.stopMongod(conn);
})();
