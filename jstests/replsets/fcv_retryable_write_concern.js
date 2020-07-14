/**
 * Tests for making sure that retried setFeatureCompatibilityVersion will wait properly for
 * writeConcern.
 *
 * @tags: [multiversion_incompatible]
 */
(function() {

"use strict";

load("jstests/libs/retryable_writes_util.js");
load("jstests/libs/write_concern_util.js");

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

const kNodes = 2;

let replTest = new ReplSetTest({nodes: kNodes});
replTest.startSet({verbose: 1});
replTest.initiate();

let priConn = replTest.getPrimary();
let secConn = replTest.getSecondary();

// Stopping replication on secondaries can take up to 5 seconds normally. Set a small oplog
// getMore timeout so the test runs faster.
assert.commandWorked(
    secConn.adminCommand({configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}));

runWriteConcernRetryabilityTest(priConn,
                                secConn,
                                {
                                    setFeatureCompatibilityVersion: lastLTSFCV,
                                    writeConcern: {w: 'majority', wtimeout: 200},
                                },
                                kNodes,
                                'admin');
assert.commandWorked(priConn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
checkFCV(priConn.getDB('admin'), lastLTSFCV);

runWriteConcernRetryabilityTest(priConn,
                                secConn,
                                {
                                    setFeatureCompatibilityVersion: latestFCV,
                                    writeConcern: {w: 'majority', wtimeout: 200},
                                },
                                kNodes,
                                'admin');
assert.commandWorked(priConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
checkFCV(priConn.getDB('admin'), latestFCV);

replTest.stopSet();
})();
