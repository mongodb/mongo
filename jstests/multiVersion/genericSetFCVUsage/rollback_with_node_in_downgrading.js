/**
 * Multiversion rollback test. Checks that rollback succeeds between a
 * latest version rollback node and a downgrading version sync source, and a
 * downgrading version rollback node and a lastLTS version sync source.
 */

(function() {
"use strict";
load("jstests/multiVersion/libs/multiversion_rollback.js");

let testName = "multiversion_rollback_latest_from_downgrading";
jsTestLog("Testing multiversion rollback with a node in latest syncing from a node in downgrading");
testMultiversionRollbackLatestFromDowngrading(testName, true /* upgradeImmediately */);
testMultiversionRollbackLatestFromDowngrading(testName, false /* upgradeImmediately */);

testName = "multiversion_rollback_downgrading_from_last_lts";
jsTestLog(
    "Testing multiversion rollback with a node in downgrading syncing from a node in lastLTS");
testMultiversionRollbackDowngradingFromLastLTS(testName);
})();
