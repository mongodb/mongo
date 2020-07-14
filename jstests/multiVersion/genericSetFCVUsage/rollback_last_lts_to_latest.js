/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * 'last-lts' version rollback node and a 'latest' version sync source.
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

var testName = "multiversion_rollback_last_lts_to_latest";
testMultiversionRollback(testName, "last-lts", "latest");
})();