// Make sure the TTL index still work after we hide it
(function() {
"use strict";
load("jstests/libs/ttl_util.js");

let runner = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
let coll = runner.getDB("test").ttl_hiddenl_index;
coll.drop();

// Create TTL index.
assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 0}));
let now = new Date();

assert.commandWorked(coll.hideIndex("x_1"));

// Insert docs after having set hidden index in order to prevent inserted docs being expired out
// before the hidden index is set.
assert.commandWorked(coll.insert({x: now}));
assert.commandWorked(coll.insert({x: now}));

// Wait for the TTL monitor to run at least twice (in case we weren't finished setting up our
// collection when it ran the first time).
TTLUtil.waitForPass(coll.getDB());

assert.eq(coll.count(), 0, "We should get 0 documents after TTL monitor run");

MongoRunner.stopMongod(runner);
})();
