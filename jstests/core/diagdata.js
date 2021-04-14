/*
 * Test that verifies getDiagnosticData returns FTDC data
 *
 * @tags: [
 *   # getDiagnosticData command is not available on embedded
 *   incompatible_with_embedded,
 *   # getDiagnosticData is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 * ]
 */

load('jstests/libs/ftdc.js');

(function() {
"use strict";

// Verify we require admin database
assert.commandFailed(db.diagdata.runCommand("getDiagnosticData"));

verifyGetDiagnosticData(db.getSiblingDB('admin'));
})();
