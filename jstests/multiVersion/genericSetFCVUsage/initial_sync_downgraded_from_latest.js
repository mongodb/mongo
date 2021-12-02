/**
 * Multiversion initial sync test. Tests that initial sync succeeds when a downgraded version
 * secondary syncs from a 'latest' version replica set.
 */

'use strict';

load("./jstests/multiVersion/libs/initial_sync.js");

let replSetVersion = "latest";

var testName = "multiversion_initial_sync_last_lts_from_latest";
jsTestLog("Testing initial sync succeeds when last-lts syncs from latest");
multversionInitialSyncTest(testName, replSetVersion, "last-lts", {}, lastLTSFCV);
