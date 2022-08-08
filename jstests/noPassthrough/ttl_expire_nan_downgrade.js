/**
 * Tests that the cluster cannot be downgraded when there are TTL indexes with
 * NaN for 'expireAfterSeconds'.
 *
 * @tags: [
 *     requires_fcv_50,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             ErrorCodes.CannotDowngrade);

assert.commandWorked(
    db.runCommand({collMod: coll.getName(), index: {name: 't_1', expireAfterSeconds: 60}}));

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

rst.stopSet();
})();
