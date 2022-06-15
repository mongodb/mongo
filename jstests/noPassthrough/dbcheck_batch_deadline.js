/**
 * Confirms that dbCheck stops processing a batch when reaching the deadline, and that
 * the following batch resumes from where the previous one left off.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

(function() {
"use strict";
load("jstests/libs/fail_point_util.js");

const replTest = new ReplSetTest({name: "dbcheck_batch_deadline", nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB('test');
const coll = db.c;
const healthlog = primary.getDB('local').system.healthlog;

const debugBuild = db.adminCommand('buildInfo').debug;

// Populate collection.
const collCount = 3;
for (let i = 0; i < collCount; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Run dbCheck with a failpoint configured so that we're only ever able to process 1 document per
// batch before hitting the 1-second default maxBatchTimeMillis.
const fp = configureFailPoint(primary, 'SleepDbCheckInBatch', {sleepMs: 2000});
const timesEntered = fp.count;
assert.commandWorked(db.runCommand({dbCheck: coll.getName()}));

// Wait for dbCheck to complete and disable the failpoint.
assert.soon(function() {
    return (healthlog.find({"operation": "dbCheckStop"}).itcount() == 1);
}, "dbCheck command didn't complete - missing healthlog entries", 30 * 1000);
fp.off();

if (debugBuild) {
    // These tests only run on debug builds because they rely on dbCheck health-logging
    // all info-level batch results.

    // Confirm each batch consists of 1 document, except for the last (maxKey) batch being empty.
    assert.eq(collCount,
              healthlog
                  .find({
                      operation: "dbCheckBatch",
                      namespace: coll.getFullName(),
                      msg: "dbCheck batch consistent",
                      "data.count": 1
                  })
                  .itcount());
    assert.eq(1,
              healthlog
                  .find({
                      operation: "dbCheckBatch",
                      namespace: coll.getFullName(),
                      msg: "dbCheck batch consistent",
                      "data.count": 0
                  })
                  .itcount());
}

assert.eq(0, healthlog.find({"severity": {$ne: "info"}}).itcount());

replTest.stopSet();
})();
