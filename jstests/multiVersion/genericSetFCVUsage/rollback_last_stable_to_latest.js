/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * 'last-stable' version rollback node and a 'latest' version sync source.
 *
 * @tags: [fix_for_fcv_46]
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

var testName = "multiversion_rollback_last_stable_to_latest";
testMultiversionRollback(testName, "last-stable", "latest");
})();