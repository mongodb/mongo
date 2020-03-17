/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * 'latest' version rollback node and a 'last-stable' version sync source.
 *
 * @tags: [fix_for_fcv_46]
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

var testName = "multiversion_rollback_latest_to_last_stable";
testMultiversionRollback(testName, "latest", "last-stable");
})();