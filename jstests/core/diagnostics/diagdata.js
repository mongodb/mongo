/*
 * Test that verifies getDiagnosticData returns FTDC data
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: getDiagnosticData,
 *   # getDiagnosticData.
 *   not_allowed_with_signed_security_token,
 *   # getDiagnosticData is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 *   # The config fuzzer may try to disable FTDC, and that would cause this test to fail.
 *   does_not_support_config_fuzzer,
 * ]
 */

import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

// Verify we require admin database
assert.commandFailed(db.diagdata.runCommand("getDiagnosticData"));

verifyGetDiagnosticData(db.getSiblingDB('admin'));
