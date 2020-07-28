/**
 * Multiversion initial sync test. Tests that initial sync succeeds when a 'latest' version
 * secondary syncs from a downgraded version replica set.
 */

'use strict';

load("./jstests/multiVersion/libs/initial_sync.js");

let newSecondaryVersion = "latest";

var testName = "multiversion_initial_sync_latest_from_last_lts";
jsTestLog("Testing that initial sync succeeds when latest syncs from last-lts");
multversionInitialSyncTest(testName, "last-lts", newSecondaryVersion, {}, lastLTSFCV);

testName = "multiversion_initial_sync_latest_from_last_continuous";
jsTestLog("Testing that initial sync succeeds when latest syncs from last-continuous");
multversionInitialSyncTest(testName, "last-continuous", newSecondaryVersion, {}, lastContinuousFCV);
