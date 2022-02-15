/**
 * Validate that under significant WiredTiger cache pressure an operation can fail
 * with TemporarilyUnavailable error.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   does_not_support_stepdowns,
 *   // Exclude in-memory engine, rollbacks due to pinned cache content rely on eviction.
 *   requires_journaling,
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

const mongod =
    MongoRunner.runMongod({wiredTigerCacheSizeGB: 0.256, setParameter: "loadShedding=1"});
const db = mongod.getDB("test");

// Generate a workload pinning enough dirty data in cache that causes WiredTiger
// to roll back transactions. This workload is adapted from the reproducer in the
// SERVER-61909 ticket description.
assert.commandWorked(db.c.createIndex({x: "text"}));
let doc = {x: []};
for (let j = 0; j < 50000; j++)
    doc.x.push("" + Math.random() + Math.random());

let caughtTUerror = false;
let attempts;
for (attempts = 1; attempts <= 20; attempts++) {
    const ret = db.c.insert(doc);

    if (ret["nInserted"] === 1) {
        // The write succeeded.
        continue;
    }
    assert.eq(0, ret["nInserted"]);
    assert.commandFailedWithCode(ret, ErrorCodes.TemporarilyUnavailable);
    caughtTUerror = true;
    jsTestLog("returned the expected TemporarilyUnavailable code at attempt " + attempts);
    break;
}

assert.eq(true,
          caughtTUerror,
          "did not return the expected TemporarilyUnavailable error after " + (attempts - 1) +
              " attempts");

MongoRunner.stopMongod(mongod);
})();
