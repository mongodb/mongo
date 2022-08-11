/**
 * Tests that the cluster can be downgraded when there are TTL indexes with
 * NaN for 'expireAfterSeconds'.
 *
 * @tags: [
 *     requires_fcv_50,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

// The test cases here revolve around having a TTL index in the catalog with a NaN
// 'expireAfterSeconds'. The current createIndexes behavior will overwrite NaN with int32::max
// unless we use a fail point.
const fp = configureFailPoint(primary, 'skipTTLIndexNaNExpireAfterSecondsValidation');
try {
    assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
} finally {
    fp.off();
}

assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

rst.stopSet();
})();
