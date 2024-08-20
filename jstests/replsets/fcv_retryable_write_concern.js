/**
 * Tests for making sure that retried setFeatureCompatibilityVersion will wait properly for
 * writeConcern.
 *
 * @tags: [multiversion_incompatible]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {runWriteConcernRetryabilityTest} from "jstests/libs/write_concern_util.js";

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
                                    confirm: true,
                                    writeConcern: {w: 'majority', wtimeout: 200},
                                },
                                kNodes,
                                'admin');
assert.commandWorked(
    priConn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
checkFCV(priConn.getDB('admin'), lastLTSFCV);

runWriteConcernRetryabilityTest(priConn,
                                secConn,
                                {
                                    setFeatureCompatibilityVersion: latestFCV,
                                    confirm: true,
                                    writeConcern: {w: 'majority', wtimeout: 200},
                                },
                                kNodes,
                                'admin');
assert.commandWorked(
    priConn.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
checkFCV(priConn.getDB('admin'), latestFCV);

replTest.stopSet();