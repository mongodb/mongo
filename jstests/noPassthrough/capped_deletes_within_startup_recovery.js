/**
 * Tests that capped deletes occur during statup recovery on documents inserted earlier in startup
 * recovery.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection(jsTestName());

assert.commandWorked(testDB.createCollection(coll.getName(), {capped: true, size: 100, max: 1}));

const ts = assert.commandWorked(testDB.runCommand({insert: coll.getName(), documents: [{a: 1}]}))
               .operationTime;
configureFailPoint(primary, 'holdStableTimestampAtSpecificTimestamp', {timestamp: ts});

assert.commandWorked(coll.insert([{b: 1}, {b: 2}]));
replTest.restart(primary);

// Stopping the test fixture runs validate with {enforceFastCount: true}. This will cause collection
// validation to fail if startup recovery did not perform capped deletes on documents that were
// inserted earlier in startup recovery.
replTest.stopSet();
})();
