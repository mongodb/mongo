/*
 * Tests that we are silently ignoring writeConcern when we write to local db.
 */

(function() {
'use strict';

load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

const rst = new ReplSetTest(
    {nodes: [{}, {rsConfig: {priority: 0}}], nodeOptions: {setParameter: {logLevel: 1}}});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const primaryDB = primary.getDB("local");
const primaryColl = primaryDB["test"];

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB("local");
const secondaryColl = secondaryDB["test"];

jsTestLog("Write to local db on the secondary node should succeed.");
secondaryColl.insertOne({x: 1});
secondaryColl.insertOne({x: 2}, {writeConcern: {w: 1}});
secondaryColl.insertOne({x: 3}, {writeConcern: {w: 2}});

jsTestLog("Stop replication  to prevent primary from satisfying majority write-concern.");
stopReplicationOnSecondaries(rst, false /* changeReplicaSetDefaultWCToLocal */);

// Advance the primary opTime by doing local dummy write.
assert.commandWorked(
    rst.getPrimary().getDB("dummy")["dummy"].insert({x: 'dummy'}, {writeConcern: {w: 1}}));

jsTestLog("Write to local db on the primary node should succeed.");
primaryColl.insertOne({x: 4});
primaryColl.insertOne({x: 5}, {writeConcern: {w: 1}});
primaryColl.insertOne({x: 6}, {writeConcern: {w: 2}});

restartReplicationOnSecondaries(rst);
rst.stopSet();
})();
