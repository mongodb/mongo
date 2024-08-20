/**
 * Tests that we are silently ignoring writeConcern when we write to local db.
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    restartReplicationOnSecondaries,
    stopReplicationOnSecondaries
} from "jstests/libs/write_concern_util.js";

const rst = new ReplSetTest(
    {nodes: [{}, {rsConfig: {priority: 0}}], nodeOptions: {setParameter: {logLevel: 1}}});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const secondary = rst.getSecondary();

jsTestLog("Write to local db on the secondary node should succeed.");
secondary.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {x: 1}}, {insert: 1, document: {x: 1}}],
    nsInfos: [{ns: "local.test"}, {ns: "local.test1"}]
});
secondary.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {x: 2}}, {insert: 1, document: {x: 2}}],
    nsInfos: [{ns: "local.test"}, {ns: "local.test1"}],
    writeConcern: {w: 1}
});
secondary.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {x: 3}}, {insert: 1, document: {x: 3}}],
    nsInfos: [{ns: "local.test"}, {ns: "local.test1"}],
    writeConcern: {w: 2}
});

jsTestLog("Stop replication  to prevent primary from satisfying majority write-concern.");
stopReplicationOnSecondaries(rst, false /* changeReplicaSetDefaultWCToLocal */);

// Advance the primary opTime by doing local dummy write.
assert.commandWorked(
    rst.getPrimary().getDB("dummy")["dummy"].insert({x: 'dummy'}, {writeConcern: {w: 1}}));

jsTestLog("Write to local db on the primary node should succeed.");
primary.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {x: 4}}, {insert: 1, document: {x: 4}}],
    nsInfos: [{ns: "local.test"}, {ns: "local.test1"}]
});
primary.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {x: 5}}, {insert: 1, document: {x: 5}}],
    nsInfos: [{ns: "local.test"}, {ns: "local.test1"}],
    writeConcern: {w: 1}
});
primary.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {x: 6}}, {insert: 1, document: {x: 6}}],
    nsInfos: [{ns: "local.test"}, {ns: "local.test1"}],
    writeConcern: {w: 2}
});

restartReplicationOnSecondaries(rst);
rst.stopSet();
