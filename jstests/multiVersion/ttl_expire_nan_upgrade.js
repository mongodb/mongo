/**
 * Tests that upgrading a server containing a TTL index with NaN for 'expireAfterSeconds'
 * is supported.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [{binVersion: 'last-lts'}, {binVersion: 'last-lts', rsConfig: {votes: 0, priority: 0}}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const db = primary.getDB('test');
const coll = db.t;

assert.commandWorked(coll.createIndex({t: 1}, {expireAfterSeconds: NaN}));
assert.commandWorked(coll.insert({_id: 0, t: ISODate()}));

// Force checkpoint in storage engine to ensure index is part of the catalog in
// in finished state at startup.
rst.awaitReplication();
let secondary = rst.getSecondary();
assert.commandWorked(secondary.adminCommand({fsync: 1}));

// Restart the secondary with a 5.0+ binary. The node is expected to complete its startup procedure.
secondary = rst.restart(secondary, {binVersion: 'latest'});
rst.waitForState(secondary, ReplSetTest.State.SECONDARY);
rst.awaitReplication();
const secondaryColl = secondary.getCollection(coll.getFullName());
const secondaryIndexes = IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 't_1']);
const secondaryTTLIndex = secondaryIndexes.t_1;
assert(secondaryTTLIndex.hasOwnProperty('expireAfterSeconds'), tojson(secondaryTTLIndex));
assert.gt(secondaryTTLIndex.expireAfterSeconds, 0, tojson(secondaryTTLIndex));

rst.stopSet();
})();
