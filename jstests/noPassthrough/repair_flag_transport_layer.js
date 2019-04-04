/*
 * SERVER-28990: Tests that a merizod started with --repair doesn't attempt binding to a port.
 */

(function() {
    "use strict";
    let dbpath = MerizoRunner.dataPath + "repair_flag_transport_layer";
    resetDbpath(dbpath);

    function runTest(conn) {
        let returnCode =
            runNonMerizoProgram("merizod", "--port", conn.port, "--repair", "--dbpath", dbpath);
        assert.eq(
            returnCode, 0, "expected merizod --repair to execute successfully regardless of port");
    }

    let conn = MerizoRunner.runMerizod();

    runTest(conn);
    MerizoRunner.stopMerizod(conn);

})();
