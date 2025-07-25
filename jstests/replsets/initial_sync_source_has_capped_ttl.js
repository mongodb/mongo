/**
 * Tests initial sync succeeds on a node with a capped collection with a TTL index.
 *
 * This is a regression test for SERVER-104771.
 *
 * @tags: [
 *   uses_full_validation,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/replsets/rslib.js');  // waitForState.

const dbName = jsTestName();
const ttlMonitorSleepSecs = 1;  // Shorten TTL monitor sleep time for faster tests.
const replTest = new ReplSetTest({
    name: jsTestName(),
    nodes: 1,
    nodeOptions: {
        setParameter: {
            "failpoint.ignoreTTLIndexCappedCollectionCheck": tojson({mode: "alwaysOn"}),
            ttlMonitorSleepSecs: ttlMonitorSleepSecs,
        }
    }
});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const primaryDB = primary.getDB(dbName);
const cappedCollName = "capped_coll";
const primaryCappedColl = primaryDB[cappedCollName];

assert.commandWorked(primaryDB.createCollection(cappedCollName, {capped: true, size: 100000}));
assert.commandWorked(primaryCappedColl.createIndex({bar: 1}, {expireAfterSeconds: 1}));

assert.commandWorked(
    primary.adminCommand({configureFailPoint: "ignoreTTLIndexCappedCollectionCheck", mode: "off"}));

// With the failpoint off, TTL index creation on capped collections should not be possible.
assert.commandFailedWithCode(primaryCappedColl.createIndex({baz: 1}, {expireAfterSeconds: 10}),
                             ErrorCodes.CannotCreateIndex);

// Insert a document into the capped collection that may be deleted by the TTL index.
assert.commandWorked(primaryCappedColl.insert({foo: 1, bar: new Date()}));

replTest.add({
    rsConfig: {votes: 0, priority: 0},
    setParameter: {
        ttlMonitorSleepSecs: ttlMonitorSleepSecs,
    }
});

const secondary = replTest.getSecondary();

// Initial sync.
replTest.reInitiate();
replTest.awaitReplication();

// If the secondary is not in the SECONDARY state, the validate command will fail.
waitForState(secondary, ReplSetTest.State.SECONDARY);

// Make sure the TTL index was created during initial sync and is valid.
const secondaryCappedColl = secondary.getDB(dbName)[cappedCollName];
const indexes = secondaryCappedColl.getIndexes();

assert.eq(
    indexes.length,
    2,
    "Expected 2 indexes on " + secondaryCappedColl.name + " (_id and TTL): " + tojson(indexes));
const ttlIndex = indexes.find(index => index.expireAfterSeconds !== undefined);
assert(ttlIndex, "Expected to find a TTL index in: " + tojson(indexes));

const validate_result = secondaryCappedColl.validate({full: true});
const failMsg =
    "Index validation of '" + secondaryCappedColl.name + "' failed: " + tojson(validate_result);
assert(validate_result.valid, failMsg);

// Wait for a few seconds to ensure that the TTL index does not delete any documents.
sleep(ttlMonitorSleepSecs * 1000 * 5);
assert.eq(primaryCappedColl.find({foo: 1}).itcount(),
          1,
          "TTL index should not delete documents in capped collection on primary");
assert.eq(secondaryCappedColl.find({foo: 1}).itcount(),
          1,
          "TTL index should not delete documents in capped collection on secondary");

replTest.stopSet();
})();
