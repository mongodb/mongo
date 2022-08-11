/**
 * Tests that adding a node running a 5.0+ binary to an existing 4.4 cluster containing
 * a TTL index with NaN for 'expireAfterSeconds' is supported.
 *
 * @tags: [
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{binVersion: 'last-lts'}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

const newNode = rst.add({
    binVersion: 'latest',
    rsConfig: {votes: 0, priority: 0},
    setParameter: {numInitialSyncAttempts: 1},
});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
const newNodeColl = newNode.getCollection(coll.getFullName());
const newNodeIndexes = IndexBuildTest.assertIndexes(newNodeColl, 2, ['_id_', 't_1']);
const newNodeTTLIndex = newNodeIndexes.t_1;
assert(newNodeTTLIndex.hasOwnProperty('expireAfterSeconds'), tojson(newNodeTTLIndex));
assert.gt(newNodeTTLIndex.expireAfterSeconds, 0, tojson(newNodeTTLIndex));

rst.stopSet();
})();
