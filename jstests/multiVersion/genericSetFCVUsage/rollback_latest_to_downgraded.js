/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * 'latest' version rollback node and a downgraded version sync source.
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

var testName = "multiversion_rollback_latest_to_last_lts";
jsTestLog("Testing multiversion rollback from latest to last-lts");
testMultiversionRollback(testName, "latest", "last-lts");

var testName = "multiversion_rollback_latest_to_last_continuous";
jsTestLog("Testing multiversion rollback from latest to last-continuous");
testMultiversionRollback(testName, "latest", "last-continuous");
})();
