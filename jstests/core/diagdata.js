// Test that verifies getDiagnosticData returns FTDC data

(function() {
    "use strict";

    // Verify we require admin database
    assert.commandFailed(db.diagdata.runCommand("getDiagnosticData"));

    // We need to retry a few times if run this test immediately after mongod is started as FTDC may
    // not have run yet.
    var foundGoodDocument = false;

    for (var i = 0; i < 60; ++i) {
        var result = db.adminCommand("getDiagnosticData");
        assert.commandWorked(result);

        var data = result.data;

        if (!data.hasOwnProperty("start")) {
            // Wait a little longer for FTDC to start
            sleep(500);
        } else {
            // Check for a few common properties to ensure we got data
            assert(data.hasOwnProperty("serverStatus"),
                   "does not have 'serverStatus' in '" + tojson(data) + "'");
            assert(data.hasOwnProperty("end"), "does not have 'end' in '" + tojson(data) + "'");
            foundGoodDocument = true;
        }
    }
    assert(foundGoodDocument,
           "getDiagnosticData failed to return a non-empty command, is FTDC running?");

})();
