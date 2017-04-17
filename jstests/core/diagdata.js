// Test that verifies getDiagnosticData returns FTDC data

(function() {
    "use strict";

    // Verify we require admin database
    assert.commandFailed(db.diagdata.runCommand("getDiagnosticData"));

    var result = db.adminCommand("getDiagnosticData");
    assert.commandWorked(result);

    var data = result.data;

    // Check for a few common properties to ensure we got data
    assert(data.hasOwnProperty("start"));
    assert(data.hasOwnProperty("serverStatus"));
    assert(data.hasOwnProperty("end"));

})();
