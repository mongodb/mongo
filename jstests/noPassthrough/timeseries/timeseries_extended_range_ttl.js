/**
 * Tests that time-series collections requiring extended range support do not perform TTL deletes.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {TTLUtil} from "jstests/libs/ttl_util.js";

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

const timeFieldName = "t";
const normalTime = ISODate("2010-01-01T00:00:00.000Z");
const extendedTime = ISODate("2040-01-01T00:00:00.000Z");

assert.commandWorked(db().createCollection(
    coll().getName(), {timeseries: {timeField: timeFieldName}, expireAfterSeconds: 3600}));

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 1}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 0);

assert.commandWorked(coll().insert({[timeFieldName]: extendedTime, doc: 2}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 1);

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 3}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 2);

replTest.restart(primary(), {skipValidation: true});

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 4}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 3);

assert.commandWorked(coll().remove({doc: 2}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 2);

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 5}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 3);

replTest.restart(primary(), {skipValidation: true});

TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 0);

assert.commandWorked(coll().insert({[timeFieldName]: normalTime, doc: 6}));
TTLUtil.waitForPass(db());
assert.eq(coll().find().itcount(), 0);

replTest.stopSet(null);
