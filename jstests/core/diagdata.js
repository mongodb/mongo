// Test that verifies getDiagnosticData returns FTDC data
load('jstests/libs/ftdc.js');

(function() {
    "use strict";

    // Verify we require admin database
    assert.commandFailed(db.diagdata.runCommand("getDiagnosticData"));

    verifyGetDiagnosticData(db.getSiblingDB('admin'));
})();
