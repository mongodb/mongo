/**
 * Tests TTL indexes with NaN for 'expireAfterSeconds'.
 *
 * Existing TTL indexes from older versions of the server may contain a NaN for the duration.
 * Newer server versions (5.0+) normalize the TTL duration to 0.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {votes: 0, priority: 0}}],
    nodeOptions: {setParameter: {ttlMonitorSleepSecs: 5}},
    // Sync from primary only so that we have a well-defined node to check listIndexes behavior.
    settings: {chainingAllowed: false},
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// Wait for "TTL indexes require the expire field to be numeric, skipping TTL job" log message.
checkLog.containsJson(primary, 22542, {ns: coll.getFullName()});

// TTL index should be replicated to the secondary with a NaN 'expireAfterSeconds'.
const secondary = rst.getSecondary();
checkLog.containsJson(secondary, 20384, {
    namespace: coll.getFullName(),
    properties: (spec) => {
        jsTestLog('TTL index on secondary: ' + tojson(spec));
        return isNaN(spec.expireAfterSeconds);
    }
});

assert.eq(
    coll.countDocuments({}), 1, 'ttl index with NaN duration should not remove any documents.');

// Confirm that TTL index is replicated with a non-zero 'expireAfterSeconds' during initial sync.
const newNode = rst.add({rsConfig: {votes: 0, priority: 0}});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
let newNodeTestDB = newNode.getDB(db.getName());
let newNodeColl = newNodeTestDB.getCollection(coll.getName());
const newNodeIndexes = IndexBuildTest.assertIndexes(newNodeColl, 2, ['_id_', 't_1']);
const newNodeSpec = newNodeIndexes.t_1;
jsTestLog('TTL index on initial sync node: ' + tojson(newNodeSpec));
assert(newNodeSpec.hasOwnProperty('expireAfterSeconds'),
       'Index was not replicated as a TTL index during initial sync.');
assert.gt(newNodeSpec.expireAfterSeconds,
          0,
          'NaN expireAferSeconds was replicated as zero during initial sync.');

// Check that listIndexes on the primary logged a "Fixing expire field from TTL index spec" message
// during the NaN 'expireAfterSeconds' conversion.
checkLog.containsJson(primary, 6835900, {namespace: coll.getFullName()});

rst.stopSet();
})();
