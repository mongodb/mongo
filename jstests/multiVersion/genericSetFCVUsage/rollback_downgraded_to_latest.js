/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * downgraded version rollback node and a 'latest' version sync source.
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

var testName = "multiversion_rollback_last_lts_to_latest";
jsTestLog("Testing multiversion rollback from last-lts to latest");
testMultiversionRollback(testName, "last-lts", "latest");

testName = "multiversion_rollback_last_continuous_to_latest";
jsTestLog("Testing multiversion rollback from last-continuous to latest");
testMultiversionRollback(testName, "last-continuous", "latest");
})();
