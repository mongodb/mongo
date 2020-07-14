/**
 * Multiversion initial sync test. Tests that initial sync succeeds when a 'latest' version
 * secondary syncs from a 'last-lts' version replica set.
 */

'use strict';

load("./jstests/multiVersion/libs/initial_sync.js");

var testName = "multiversion_initial_sync_latest_from_last_lts";
let replSetVersion = "last-lts";
let newSecondaryVersion = "latest";

multversionInitialSyncTest(testName, replSetVersion, newSecondaryVersion, {}, lastLTSFCV);
