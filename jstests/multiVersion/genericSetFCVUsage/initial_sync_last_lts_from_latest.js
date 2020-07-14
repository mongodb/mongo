/**
 * Multiversion initial sync test. Tests that initial sync succeeds when a 'last-lts' version
 * secondary syncs from a 'latest' version replica set.
 */

'use strict';

load("./jstests/multiVersion/libs/initial_sync.js");

var testName = "multiversion_initial_sync_last_lts_from_latest";
let replSetVersion = "latest";
let newSecondaryVersion = "last-lts";

multversionInitialSyncTest(testName, replSetVersion, newSecondaryVersion, {}, lastLTSFCV);
