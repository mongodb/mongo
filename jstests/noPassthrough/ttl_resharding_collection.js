// Tests that the TTL Monitor is disabled for <database>.system.resharding.* namespaces.
(function() {
"use strict";
load("jstests/libs/ttl_util.js");

// Launch mongod with shorter TTL monitor sleep interval.
const runner = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
const collName = "system.resharding.mycoll";
const coll = runner.getDB(jsTestName())[collName];
coll.drop();

assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 0}));

const now = new Date();
assert.commandWorked(coll.insert({x: now}));

// Wait for the TTL monitor to run at least twice (in case we weren't finished setting up our
// collection when it ran the first time).
TTLUtil.waitForPass(coll.getDB());

// Confirm the document was not removed because it was in a <database>.system.resharding.*
// namespace.
assert.eq(
    1, coll.find().itcount(), "Wrong number of documents in collection, after TTL monitor run");
MongoRunner.stopMongod(runner);
})();
