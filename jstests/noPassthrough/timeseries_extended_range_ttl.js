/**
 * Tests that time-series collections requiring extended range support do not perform TTL deletes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const replTest = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {ttlMonitorSleepSecs: 1}}});
replTest.startSet();
replTest.initiate();

const primary = function() {
    return replTest.getPrimary();
};
const db = function() {
    return primary().getDB(jsTestName());
};
const coll = function() {
    return db().coll;
};
const bucketsColl = function() {
    return db().system.buckets.coll;
};

const timeFieldName = "t";
const metaFieldName = "m";
const normalTime = ISODate("2010-01-01T00:00:00.000Z");
const extendedTime = ISODate("2040-01-01T00:00:00.000Z");

assert.commandWorked(db().createCollection(
    coll().getName(),
    {timeseries: {timeField: timeFieldName, metaField: metaFieldName}, expireAfterSeconds: 3600}));

const waitForTTL = function() {
    const passes = db().serverStatus().metrics.ttl.passes;
    assert.soon(function() {
        return db().serverStatus().metrics.ttl.passes > passes;
    });
};

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 1}));
waitForTTL();
assert.eq(coll().find().itcount(), 0);

assert.commandWorked(coll().insert({[timeFieldName]: extendedTime, [metaFieldName]: 0, doc: 2}));
waitForTTL();
assert.eq(coll().find().itcount(), 1);

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 3}));
waitForTTL();
assert.eq(coll().find().itcount(), 2);

replTest.restart(primary());

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 4}));
waitForTTL();
assert.eq(coll().find().itcount(), 3);

assert.commandWorked(bucketsColl().remove({meta: 0}));
waitForTTL();
assert.eq(coll().find().itcount(), 2);

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 5}));
waitForTTL();
assert.eq(coll().find().itcount(), 3);

replTest.restart(primary());

waitForTTL();
assert.eq(coll().find().itcount(), 0);

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 6}));
waitForTTL();
assert.eq(coll().find().itcount(), 0);

replTest.stopSet();
})();
