// Ensure that changes to the TTL sleep time are reflected immediately.
(function() {
"use strict";
load("jstests/libs/ttl_util.js");

let runner = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1000"});
let db = runner.getDB("test");
let coll = db.ttl_collection;
coll.drop();

// Create TTL index.
assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 0}));

// Insert expired docs.
let now = new Date();
assert.commandWorked(coll.insert({x: now}));
assert.commandWorked(coll.insert({x: now}));

// TTL monitor should now be waiting for 1000 seconds. Modify it to 1 second.
assert.commandWorked(db.adminCommand({setParameter: 1, ttlMonitorSleepSecs: 1}));

// TTL Monitor should now perform passes every second. A timeout here would mean we fail the test.
TTLUtil.waitForPass(coll.getDB(), true, 20 * 1000);

assert.eq(coll.count(), 0, "We should get 0 documents after TTL monitor run");

MongoRunner.stopMongod(runner);
})();
