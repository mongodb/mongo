// Test that verifies getDiagnosticDataFiles returns FTDC data files

(function() {
    "use strict";

    // Verify we require admin database
    assert.commandFailed(db.diagdata.runCommand("getDiagnosticDataFiles"));

    // We need to retry a few times if run this test immediately after mongod is started as FTDC may
    // not have run yet so it may have no files.
    var foundFiles = false;

    for (var i = 0; i < 60; ++i) {
        var result = db.adminCommand("getDiagnosticDataFiles");
        assert.commandWorked(result);

        var data = result.data;
        //Check for the metrics.interim file it should be present with a good probability
        if (data.indexOf("metrics.interim") > -1) {
            foundFiles = true;
        } else {
            // Wait a little longer for FTDC to start
            sleep(500);
        }
    }
    assert(foundFiles,
           "getDiagnosticDataFiles failed to return files, is FTDC running?");
})();
